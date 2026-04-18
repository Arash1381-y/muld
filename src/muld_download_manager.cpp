
#include "muld_download_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

#include "connection_controller.h"
#include "downloader.h"
#include "task.h"
#include "threadpool.h"
#include "url.h"

constexpr int MAX_REDIRECT_ALLOW = 3;
constexpr size_t MIN_CHUNK_SIZE = 1 * 1024 * 1024;   // 1 MB
constexpr size_t MAX_CHUNK_SIZE = 10 * 1024 * 1024;  // 10 MB
constexpr int PER_JOB_CONNECTION_CAP = 8;
constexpr std::chrono::milliseconds CONNECTION_CONTROL_TICK(2000);

namespace muld {

namespace {

std::optional<FileInfo> ResolveValidatedFileInfo(const Url& initial_url,
                                                 Url& resolved_url) {
  FetchResult fetch_res;
  resolved_url = initial_url;

  for (int redirect_count = 0; redirect_count < MAX_REDIRECT_ALLOW;
       redirect_count++) {
    fetch_res = NetDownloader::FetchFileInfo(resolved_url);
    if (fetch_res.state == FetchResult::State::SUCCESSFUL) {
      return std::get<FileInfo>(fetch_res.data);
    }
    if (fetch_res.state != FetchResult::State::REDIRECT) {
      return std::nullopt;
    }

    resolved_url = ParseUrl(std::get<FetchRedirect>(fetch_res.data).new_url);
  }

  return std::nullopt;
}

bool MatchesStoredFile(const JobImage& img, const FileInfo& info) {
  if (img.file_size > 0 && info.total_size > 0 &&
      img.file_size != info.total_size) {
    return false;
  }
  if (!img.etag.empty() && !info.etag.empty() && img.etag != info.etag) {
    return false;
  }
  if (!img.last_modified.empty() && !info.last_modified.empty() &&
      img.last_modified != info.last_modified) {
    return false;
  }
  return true;
}

int ComputeMaxConnections(bool supports_range, std::size_t speed_limit_bps,
                          int global_capacity) {
  if (!supports_range || global_capacity <= 1) {
    return 1;
  }

  const int cap = std::max(1, std::min(global_capacity, PER_JOB_CONNECTION_CAP));
  if (speed_limit_bps == 0) {
    return cap;
  }

  // Heuristic: one connection can usually sustain around 1 MiB/s on typical
  // WAN links. Keep one extra connection for latency hiding.
  constexpr double kPerConnectionTargetBps = 1024.0 * 1024.0;
  const int by_speed =
      static_cast<int>(std::ceil(static_cast<double>(speed_limit_bps) /
                                 kPerConnectionTargetBps)) +
      1;
  return std::clamp(by_speed, 1, cap);
}

int ComputeInitialDesiredConnections(std::size_t speed_limit_bps,
                                     int max_connections) {
  if (max_connections <= 1) {
    return 1;
  }
  if (speed_limit_bps > 0 && speed_limit_bps <= 512 * 1024) {
    return 1;
  }
  return std::min(2, max_connections);
}

}  // namespace

MuldDownloadManager::MuldDownloadManager(const MuldConfig& config)
    : logger_(config.logger),
      max_threads_(std::max(1, config.max_threads)),
      jobs_mtx_(),
      jobs_(),
      jobs_index_(),
      loaded_images_(),
      threadpool_(std::make_unique<ThreadPool>(
          max_threads_,
          [](const Task& task) { NetDownloader::DownloadWorker(task); })),
      connection_controller_(std::make_unique<ConnectionController>(
          max_threads_)) {
  dispatcher_thread_ = std::thread([this]() { DownloadDispatcherLoop(); });
  controller_thread_ = std::thread([this]() { ConnectionControlLoop(); });
}

MuldDownloadManager::~MuldDownloadManager() {
  {
    std::lock_guard<std::mutex> lock(pending_mtx_);
    stop_dispatcher_ = true;
  }
  pending_cv_.notify_all();
  {
    std::lock_guard<std::mutex> lock(controller_mtx_);
    stop_controller_ = true;
  }
  controller_cv_.notify_all();
  if (dispatcher_thread_.joinable()) {
    dispatcher_thread_.join();
  }
  if (controller_thread_.joinable()) {
    controller_thread_.join();
  }
}

void MuldDownloadManager::EnqueueTasks(DownloadEngine* job) {
  if (!job) {
    return;
  }
  const int num_connections = job->PlanNewWorkers(max_threads_);
  for (int i = 0; i < num_connections; i++) {
    threadpool_->Enqueue({.job = job, .logger = this->logger_});
  }
}

DownloaderResp MuldDownloadManager::Download(
    const MuldRequest& request, const DownloadCallbacks& callbacks) {
  if (!request.url || !request.destination) {
    return {{.code = ErrorCode::InvalidRequest,
             .detail = "URL and destination must not be null"},
            {}};
  }

  Url parsed_url;
  try {
    parsed_url = ParseUrl(request.url);
  } catch (const std::exception& e) {
    return {{.code = ErrorCode::InvalidRequest, .detail = e.what()}, {}};
  }

  DownloadHandler task(parsed_url, request.destination, callbacks);
  task.Resume();
  task.SetSpeedLimit(request.speed_limit_bps);
  {
    std::lock_guard<std::mutex> lock(pending_mtx_);
    pending_requests_.push(PendingDownloadRequest{
        .kind = PendingDownloadRequest::Kind::Download,
        .url = parsed_url,
        .destination = request.destination,
        .speed_limit_bps = request.speed_limit_bps,
        .image_path = {},
        .callbacks = callbacks,
        .handler = task,
    });
  }
  {
    std::lock_guard<std::mutex> lock(jobs_mtx_);
    handler_.push_back(task);
  }
  pending_cv_.notify_one();

  return {MuldError(), task};
}

void MuldDownloadManager::WaitAll() {
  std::vector<DownloadHandler> local_tasks;
  {
    std::lock_guard<std::mutex> lock(jobs_mtx_);
    local_tasks = handler_;
  }
  for (auto& task : local_tasks) {
    task.WaitUntilFinished();
  }
}

void MuldDownloadManager::Terminate() {
  std::vector<std::shared_ptr<DownloadEngine>> local_jobs;
  {
    std::lock_guard<std::mutex> lock(jobs_mtx_);
    local_jobs = jobs_;
  }
  for (const auto& j : local_jobs) {
    if (!j->IsFinished()) {
      j->Store();
      // TODO: pause or cancel?
      j->Pause();
    }
  }
}

DownloaderResp MuldDownloadManager::Load(const std::string& path,
                                         const DownloadCallbacks& callbacks) {
  if (path.empty()) {
    return {MuldError{.code = ErrorCode::InvalidRequest,
                      .detail = "Path must not be empty"},
            {}};
  }

  DownloadHandler task(Url{}, path, callbacks);
  {
    std::lock_guard<std::mutex> lock(pending_mtx_);
    pending_requests_.push(PendingDownloadRequest{
        .kind = PendingDownloadRequest::Kind::Load,
        .url = Url{},
        .destination = {},
        .speed_limit_bps = 0,
        .image_path = path,
        .callbacks = callbacks,
        .handler = task,
    });
  }
  pending_cv_.notify_one();

  {
    std::lock_guard<std::mutex> lock(jobs_mtx_);
    handler_.push_back(task);
  }
  return {MuldError(), task};
}

void MuldDownloadManager::DownloadDispatcherLoop() {
  while (true) {
    std::optional<PendingDownloadRequest> pending;
    {
      std::unique_lock<std::mutex> lock(pending_mtx_);
      pending_cv_.wait(
          lock, [this]() { return stop_dispatcher_ || !pending_requests_.empty(); });
      if (stop_dispatcher_ && pending_requests_.empty()) {
        return;
      }
      pending = std::move(pending_requests_.front());
      pending_requests_.pop();
    }

    if (!pending.has_value()) {
      continue;
    }

    if (pending->kind == PendingDownloadRequest::Kind::Load) {
      const std::string path = pending->image_path;
      JobImage img;
      if (!ReadImageFromDisk(img, path)) {
        pending->handler.FailBeforeEngineStart(ErrorCode::DiskError,
                                            "Can not load download");
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        if (!loaded_images_.insert(path).second) {
          pending->handler.FailBeforeEngineStart(ErrorCode::DuplicateJob,
                                              "Job already loaded");
          continue;
        }
      }

      Url corrected_url;
      std::optional<FileInfo> info;
      try {
        info = ResolveValidatedFileInfo(ParseUrl(img.url), corrected_url);
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        loaded_images_.erase(path);
        pending->handler.FailBeforeEngineStart(ErrorCode::InvalidRequest, e.what());
        continue;
      }

      if (!info.has_value()) {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        loaded_images_.erase(path);
        pending->handler.FailBeforeEngineStart(ErrorCode::FetchFileInfoFailed,
                                            "Failed to fetch URL");
        continue;
      }

      if (!MatchesStoredFile(img, *info)) {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        loaded_images_.erase(path);
        pending->handler.FailBeforeEngineStart(
            ErrorCode::FetchFileInfoFailed,
            "Attachment does not match with current content");
        continue;
      }

      img.url = GetUrlString(corrected_url);
      // original:
      img.file_size = info->total_size;
      // debugging:
      // img.file_size = 1024 * 1024 * 40; // 40MB
      img.ranged = info->supports_range;
      img.etag = info->etag;
      img.last_modified = info->last_modified;

      const std::string identity_key = img.file_path + "\n" + img.url;
      {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        auto existing = jobs_index_.find(identity_key);
        if (existing != jobs_index_.end() && !existing->second.expired()) {
          loaded_images_.erase(path);
          pending->handler.FailBeforeEngineStart(
              ErrorCode::FetchFileInfoFailed,
              "Attachment does not match with current content");
          continue;
        }
      }

      if (!std::filesystem::exists(img.file_path)) {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        loaded_images_.erase(path);
        pending->handler.FailBeforeEngineStart(ErrorCode::DiskError,
                                            "Target file is missing on disk");
        continue;
      }

      auto local_size = std::filesystem::file_size(img.file_path);
      if (img.file_size > 0 && local_size != img.file_size) {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        loaded_images_.erase(path);
        pending->handler.FailBeforeEngineStart(
            ErrorCode::DiskError,
            "Target file size does not match stored job image");
        continue;
      }

      auto job = std::make_shared<DownloadEngine>(
          img, [this](DownloadEngine* job) {
            this->EnqueueTasks(job);
          });
      const int max_connections =
          ComputeMaxConnections(img.ranged, pending->speed_limit_bps, max_threads_);
      const int resume_hint = std::max(1, img.max_connections);
      const int initial_connections =
          std::clamp(resume_hint, 1, max_connections);
      job->SetConnectionBounds(1, max_connections);
      job->SetDesiredConnections(initial_connections);
      job->SetSpeedLimit(pending->speed_limit_bps);

      {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        jobs_.push_back(job);
        jobs_index_[job->GetIdentityKey()] = job;
      }
      pending->handler.AttachEngine(job);
      controller_cv_.notify_one();
      continue;
    }

    Url parsed_url = pending->url;
    FetchResult fetch_res;
    bool unsupported_scheme = false;
    for (int redirect_count = 0; redirect_count < MAX_REDIRECT_ALLOW;
         redirect_count++) {
      if (logger_) {
        logger_(LogLevel::Info,
                "Resolving and connecting to " + parsed_url.host + "...");
      }

      if (parsed_url.scheme == "http" || parsed_url.scheme == "https") {
        fetch_res = NetDownloader::FetchFileInfo(parsed_url);
      } else {
        unsupported_scheme = true;
        break;
      }

      if (fetch_res.state == FetchResult::State::SUCCESSFUL) {
        break;
      }
      if (fetch_res.state == FetchResult::State::REDIRECT) {
        parsed_url = ParseUrl(std::get<FetchRedirect>(fetch_res.data).new_url);
        continue;
      }
      break;
    }

    if (unsupported_scheme) {
      pending->handler.FailBeforeEngineStart(
          ErrorCode::NotSupported,
          parsed_url.scheme + " is not supported!");
      continue;
    }
    if (fetch_res.state == FetchResult::State::REDIRECT) {
      pending->handler.FailBeforeEngineStart(ErrorCode::MaxRedirectsExceeded,
                                         "Exceeded max redirects");
      continue;
    }
    if (fetch_res.state != FetchResult::State::SUCCESSFUL) {
      auto err = std::get<FetchError>(fetch_res.data);
      pending->handler.FailBeforeEngineStart(err.error_code, err.message);
      continue;
    }

    auto& info = std::get<FileInfo>(fetch_res.data);
    const int max_connections =
        ComputeMaxConnections(info.supports_range, pending->speed_limit_bps,
                              max_threads_);
    const int initial_connections =
        ComputeInitialDesiredConnections(pending->speed_limit_bps,
                                         max_connections);
    int n_chunks = 1;
    if (info.supports_range && info.total_size > 0) {
      size_t ideal_chunk_size = info.total_size / (max_connections * 4);
      size_t actual_chunk_size =
          std::max(MIN_CHUNK_SIZE, std::min(MAX_CHUNK_SIZE, ideal_chunk_size));
      n_chunks = static_cast<int>((info.total_size + actual_chunk_size - 1) /
                                  actual_chunk_size);
    }

    std::shared_ptr<DownloadEngine> job;
    try {
      job = std::make_shared<DownloadEngine>(
          parsed_url, pending->destination, max_connections,
          info.total_size, info.supports_range, n_chunks,
          [this](DownloadEngine* engine) {
            this->EnqueueTasks(engine);
          });
      job->SetConnectionBounds(1, max_connections);
      job->SetDesiredConnections(initial_connections);
      job->SetSpeedLimit(pending->speed_limit_bps);
    } catch (const std::system_error& e) {
      pending->handler.FailBeforeEngineStart(ErrorCode::DiskWriteFailed, e.what());
      continue;
    } catch (const std::exception& e) {
      pending->handler.FailBeforeEngineStart(ErrorCode::SystemError, e.what());
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(jobs_mtx_);
      jobs_.push_back(job);
      jobs_index_[job->GetIdentityKey()] = job;
    }

    pending->handler.AttachEngine(job);
    job->SetValidators(info.etag, info.last_modified);
    controller_cv_.notify_one();
  }
}

void MuldDownloadManager::ConnectionControlLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(controller_mtx_);
      controller_cv_.wait_for(lock, CONNECTION_CONTROL_TICK, [this]() {
        return stop_controller_;
      });
      if (stop_controller_) {
        return;
      }
    }

    std::vector<std::shared_ptr<DownloadEngine>> snapshot;
    {
      std::lock_guard<std::mutex> lock(jobs_mtx_);
      snapshot = jobs_;
    }

    std::vector<ConnectionControlInput> inputs;
    inputs.reserve(snapshot.size());
    for (const auto& job : snapshot) {
      if (!job) {
        continue;
      }

      const auto state = job->GetState();
      const bool active = state == DownloadState::Queued ||
                          state == DownloadState::Downloading;
      const std::uintptr_t job_id =
          reinterpret_cast<std::uintptr_t>(job.get());

      if (!active) {
        connection_controller_->Remove(job_id);
        continue;
      }

      inputs.push_back(ConnectionControlInput{
          .job_id = job_id,
          .active = true,
          .desired_connections = job->GetDesiredConnections(),
          .active_connections = job->GetActiveConnections(),
          .min_connections = job->GetMinConnections(),
          .max_connections = job->GetMaxConnections(),
          .speed_limit_bps = job->GetSpeedLimit(),
          .throughput_bps = static_cast<double>(job->GetDownloadSpeed()),
          .token_wait_ratio =
              job->ConsumeTokenWaitRatio(CONNECTION_CONTROL_TICK),
      });
    }

    if (inputs.empty()) {
      continue;
    }

    const auto decisions = connection_controller_->Tick(inputs);
    if (decisions.empty()) {
      continue;
    }

    std::unordered_map<std::uintptr_t, int> desired_by_job;
    desired_by_job.reserve(decisions.size());
    for (const auto& decision : decisions) {
      desired_by_job[decision.job_id] = decision.desired_connections;
    }

    for (const auto& job : snapshot) {
      if (!job) {
        continue;
      }
      const std::uintptr_t job_id =
          reinterpret_cast<std::uintptr_t>(job.get());
      auto it = desired_by_job.find(job_id);
      if (it == desired_by_job.end()) {
        continue;
      }
      job->SetDesiredConnections(it->second);
      EnqueueTasks(job.get());
    }
  }
}

}  // namespace muld
