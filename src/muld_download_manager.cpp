
#include "muld_download_manager.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "downloader.h"
#include "task.h"
#include "threadpool.h"
#include "url.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

constexpr int MAX_REDIRECT_ALLOW = 3;
constexpr size_t MIN_CHUNK_SIZE = 1 * 1024 * 1024;   // 1 MB
constexpr size_t MAX_CHUNK_SIZE = 10 * 1024 * 1024;  // 10 MB

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

}  // namespace

MuldDownloadManager::MuldDownloadManager(const MuldConfig& config)
    : logger_(config.logger),
      jobs_mtx_(),
      jobs_(),
      jobs_index_(),
      loaded_images_(),
      threadpool_(std::make_unique<ThreadPool>(
          config.max_threads,
          [](const Task& task) { NetDownloader::DownloadWorker(task); })) {
  dispatcher_thread_ = std::thread([this]() { DownloadDispatcherLoop(); });
}

MuldDownloadManager::~MuldDownloadManager() {
  {
    std::lock_guard<std::mutex> lock(pending_mtx_);
    stop_dispatcher_ = true;
  }
  pending_cv_.notify_all();
  if (dispatcher_thread_.joinable()) {
    dispatcher_thread_.join();
  }
}

void MuldDownloadManager::EnqueueTasks(DownloadEngine* job,
                                       int num_connections) {
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
  if (request.max_connections <= 0) {
    return {{.code = ErrorCode::InvalidRequest,
             .detail = "max_connections must be greater than zero"},
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
        .max_connections = request.max_connections,
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
        .max_connections = 1,
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
      img.file_size = info->total_size;
      img.ranged = info->supports_range;
      img.max_connections = info->supports_range ? img.max_connections : 1;
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
            this->EnqueueTasks(job, job->maxConnections_);
          });

      {
        std::lock_guard<std::mutex> lock(jobs_mtx_);
        jobs_.push_back(job);
        jobs_index_[job->GetIdentityKey()] = job;
      }
      pending->handler.AttachEngine(job);
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
    int num_connections = info.supports_range ? pending->max_connections : 1;
    int n_chunks = 1;
    if (info.supports_range && info.total_size > 0) {
      size_t ideal_chunk_size = info.total_size / (num_connections * 4);
      size_t actual_chunk_size =
          std::max(MIN_CHUNK_SIZE, std::min(MAX_CHUNK_SIZE, ideal_chunk_size));
      n_chunks = static_cast<int>((info.total_size + actual_chunk_size - 1) /
                                  actual_chunk_size);
    }

    std::shared_ptr<DownloadEngine> job;
    try {
      job = std::make_shared<DownloadEngine>(
          parsed_url, pending->destination, pending->max_connections,
          info.total_size, info.supports_range, n_chunks,
          [this](DownloadEngine* engine) {
            this->EnqueueTasks(engine, engine->maxConnections_);
          });
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
  }
}

}  // namespace muld
