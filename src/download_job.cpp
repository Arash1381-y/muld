#include "download_job.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#include "chunk_info.h"
#include "error.h"
#include "job_image.h"
#include "url.h"
#include "writer.h"

namespace muld {

namespace {
std::uint64_t GetUnixTimestamp() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
}  // namespace

DownloadJob::DownloadJob(const Url& url, const std::string& output_path,
                         int max_connections, size_t file_size, bool ranged,
                         size_t n_chunks,
                         std::function<void(DownloadJob*)> start_download,
                         const DownloadCallbacks& callbacks)
    : maxConnections_(max_connections),
      ranged_(ranged),
      url_(url),
      outputPath_(output_path),
      fileSize_(file_size),
      nTotalReceivedBytes_(0),
      nChunks_(n_chunks),
      nReceivedBytesFromLastStore_(0),
      nextWorkItem_(0),
      nDownloadedChunks_(0),
      lastSpeedCalcTime_(std::chrono::steady_clock::now()),
      lastProgressCallbackTime_(std::chrono::steady_clock::now()),
      nBytesFromLastSpeedCalc_(0),
      downloadSpeed_(0),  // (bytes / per sec)
      eta_(0),            // download job eta
      nConnections_(0),
      start_download_(start_download),
      callbacks_(callbacks) {
  state_ = DownloadState::Initialized;
  createdAt_ = GetUnixTimestamp();
  updatedAt_ = createdAt_;
  maxConnections_ = ranged_ ? maxConnections_ : 1;
  writer_ = std::make_unique<Writer>(outputPath_, fileSize_);
  chunksInfo_.resize(nChunks_);

  size_t chunk_size_org =
      static_cast<size_t>((fileSize_ + nChunks_ - 1) / nChunks_);
  for (size_t i = 0; i < nChunks_; i++) {
    auto& chunk = chunksInfo_.at(i);
    chunk.chunk_id = i;
    chunk.startRange_ = i * chunk_size_org;

    if (i == nChunks_ - 1) {
      chunk.endRange_ = fileSize_ - 1;
    } else {
      chunk.endRange_ = chunk.startRange_ + chunk_size_org - 1;
    }
  }
}

DownloadJob::DownloadJob(const JobImage& image,
                         std::function<void(DownloadJob*)> start_download,
                         const DownloadCallbacks& callbacks)
    : lastSpeedCalcTime_(std::chrono::steady_clock::now()),
      lastProgressCallbackTime_(std::chrono::steady_clock::now()),
      nBytesFromLastSpeedCalc_(0),
      downloadSpeed_(0),  // (bytes / per sec)
      eta_(0),            // download job eta
      nConnections_(0),
      nextWorkItem_(0),
      callbacks_(callbacks) {
  state_ = DownloadState::Initialized;
  url_ = ParseUrl(image.url);
  outputPath_ = image.file_path;
  fileSize_ = image.file_size;
  ranged_ = image.ranged;
  maxConnections_ = ranged_ ? image.max_connections : 1;
  etag_ = image.etag;
  lastModified_ = image.last_modified;
  createdAt_ = image.created_at;
  updatedAt_ = image.updated_at;
  writer_ = std::make_unique<Writer>(outputPath_, fileSize_);
  start_download_ = start_download;
  imageStored_ = true;

  chunksInfo_.resize(image.chunks.size());
  size_t already_downloaded = 0;
  nChunks_ = image.chunks.size();
  for (size_t i = 0; i < nChunks_; i++) {
    const auto& c = image.chunks[i];
    auto& chunk = chunksInfo_[i];
    chunk.chunk_id = i;
    chunk.startRange_ = c.start_range;
    chunk.endRange_ = c.end_range;
    chunk.UpdateReceived(c.downloaded);
    already_downloaded += c.downloaded;
  }

  // Count already-finished chunks
  size_t finished = 0;
  for (size_t i = 0; i < nChunks_; i++) {
    if (chunksInfo_[i].IsFinished()) finished++;
  }
  nDownloadedChunks_ = finished;
  nTotalReceivedBytes_ = already_downloaded;
  nReceivedBytesFromLastStore_ = already_downloaded;
}

void DownloadJob::SetValidators(const std::string& etag,
                                const std::string& last_modified) {
  etag_ = etag;
  lastModified_ = last_modified;
}

void DownloadJob::AttachCallbacks(const DownloadCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(callbacks_mtx_);
  callbacks_ = callbacks;
}

void DownloadJob::NotifyConnectionOpen() {
  nConnections_.fetch_add(1);

  auto expected = DownloadState::Queued;
  if (state_.compare_exchange_strong(expected, DownloadState::Downloading)) {
    std::function<void(DownloadState)> on_state_change_cb;
    {
      std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
      on_state_change_cb = callbacks_.on_state_change;
    }
    if (on_state_change_cb) {
      on_state_change_cb(DownloadState::Downloading);
    }
  }
}

void DownloadJob::NotifyConnectionClose() { nConnections_.fetch_sub(1); }

// used for redirection
void DownloadJob::SetUrl(const Url& url) { url_ = url; }

const Url& DownloadJob::GetUrl() const { return url_; }

bool DownloadJob::IsRanged() const { return ranged_; }

std::string DownloadJob::GetIdentityKey() const {
  return outputPath_ + "\n" + GetUrlString(url_);
}

WorkItem* DownloadJob::GetNextWorkItem() {
  size_t idx = nextWorkItem_.fetch_add(1);
  if (idx >= pendingWork_.size()) return nullptr;
  return &pendingWork_[idx];
}

ChunkInfo& DownloadJob::GetChunkInfo(size_t index) {
  if (index >= nChunks_) {
    throw std::runtime_error("Chunk index is invalid");
  }

  return chunksInfo_.at(index);
}

Writer& DownloadJob::GetWriter() { return *writer_; }
const Writer& DownloadJob::GetWriter() const { return *writer_; }

size_t DownloadJob::GetNumChunks() const { return nChunks_; }

const MuldError& DownloadJob::GetError() const { return error_; }

size_t DownloadJob::GetTotalSize() const { return fileSize_; };

size_t DownloadJob::GetReceivedSize() const {
  return nTotalReceivedBytes_.load();
};

size_t DownloadJob::GetDownloadSpeed() const { return downloadSpeed_.load(); };

size_t DownloadJob::GetJobEta() const { return eta_.load(); };

void DownloadJob::NotifyChunkReceived(size_t chunk_id, size_t bytes) {
  auto& chunk = chunksInfo_.at(chunk_id);
  chunk.UpdateReceived(bytes);

  // check the timer and update the speed if needed
  nBytesFromLastSpeedCalc_.fetch_add(bytes);
  nTotalReceivedBytes_.fetch_add(bytes);

  bool should_fire_progress = false;
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - lastSpeedCalcTime_).count();
  if (dt > 0.5) {
    std::lock_guard<std::mutex> speed_lock(speed_mtx_);
    now = std::chrono::steady_clock::now();
    dt = std::chrono::duration<double>(now - lastSpeedCalcTime_).count();
    if (dt > 0.5) {
      // compute speed and eta
      downloadSpeed_.store(nBytesFromLastSpeedCalc_ / dt);
      double speed = downloadSpeed_.load();
      if (speed > 0) {
        eta_.store((fileSize_ - nTotalReceivedBytes_) / speed);
      }

      // reset states
      nBytesFromLastSpeedCalc_ = 0;
      lastSpeedCalcTime_ = now;
      should_fire_progress = true;
    }
  }

  // Fire on_chunk_progress (lightweight, every call)
  ChunkProgressEvent evt;
  evt.chunk_id = chunk_id;
  evt.downloaded_bytes = chunk.GetReceivedSize();
  evt.total_bytes = chunk.GetTotalSize();
  evt.finished = chunk.IsFinished();
  std::function<void(const ChunkProgressEvent&)> on_chunk_progress_cb;
  {
    std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
    on_chunk_progress_cb = callbacks_.on_chunk_progress;
  }
  if (on_chunk_progress_cb) {
    on_chunk_progress_cb(evt);
  }

  // Fire throttled on_progress (~500ms)
  if (should_fire_progress) {
    DownloadProgress dp;
    dp.total_bytes = fileSize_;
    dp.downloaded_bytes = nTotalReceivedBytes_.load();
    dp.speed_bytes_per_sec = static_cast<size_t>(downloadSpeed_.load());
    dp.eta_seconds = static_cast<size_t>(eta_.load());
    dp.percentage =
        fileSize_ > 0
            ? static_cast<float>(dp.downloaded_bytes) /
                  static_cast<float>(fileSize_) * 100.0f
            : 0.0f;
    std::function<void(const DownloadProgress&)> on_progress_cb;
    {
      std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
      on_progress_cb = callbacks_.on_progress;
    }
    if (on_progress_cb) {
      on_progress_cb(dp);
    }
  }

  {
    bool should_store = false;
    {
      std::lock_guard<std::mutex> disk_lock(disk_mtx_);
      nReceivedBytesFromLastStore_ += bytes;
      should_store = NeedsStore();
    }
    if (should_store) {
      Store();
    }
  }

  if (chunk.IsFinished()) {
    if (nDownloadedChunks_.fetch_add(1, std::memory_order_acq_rel) ==
        nChunks_ - 1) {
      // All chunks done
      auto expected = DownloadState::Downloading;
      if (state_.compare_exchange_strong(expected, DownloadState::Completed)) {
        Store();
        std::function<void()> on_finish_cb;
        {
          std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
          on_finish_cb = callbacks_.on_finish;
        }
        if (on_finish_cb) {
          on_finish_cb();
        }
        std::lock_guard<std::mutex> wait_lock(wait_mtx_);
        wait_cv_.notify_all();
      } else if (expected == DownloadState::Paused ||
                 expected == DownloadState::Failed) {
        state_.store(DownloadState::Completed);
        Store();
        std::function<void()> on_finish_cb;
        {
          std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
          on_finish_cb = callbacks_.on_finish;
        }
        if (on_finish_cb) {
          on_finish_cb();
        }
        std::lock_guard<std::mutex> wait_lock(wait_mtx_);
        wait_cv_.notify_all();
      }
    }
  }
}

bool DownloadJob::IsFinished() const {
  return nDownloadedChunks_.load() == nChunks_ ||
         state_.load() == DownloadState::Canceled ||
         state_.load() == DownloadState::Paused ||
         state_.load() == DownloadState::Completed ||
         state_.load() == DownloadState::Failed;
}

void DownloadJob::WaitUntilFinished() {
  std::unique_lock<std::mutex> lock(wait_mtx_);
  wait_cv_.wait(lock, [this]() { return IsFinished(); });
}

void DownloadJob::BuildPendingWork() {
  pendingWork_.clear();
  size_t work_id = 0;
  size_t finished_count = 0;
  for (size_t i = 0; i < nChunks_; i++) {
    auto& chunk = chunksInfo_[i];
    if (chunk.IsFinished()) {
      finished_count++;
      continue;
    }
    WorkItem wi;
    wi.work_id = work_id++;
    wi.chunk_id = i;
    wi.range_start = chunk.startRange_ + chunk.GetReceivedSize();
    wi.range_end = chunk.endRange_;
    pendingWork_.push_back(wi);
  }
  nextWorkItem_ = 0;
  nDownloadedChunks_ = finished_count;
}

bool DownloadJob::Start() {
  if (!SetState(DownloadState::Queued)) return false;

  // Wait for any dangling connections to close
  while (nConnections_.load() != 0) {
    std::this_thread::yield();
  }

  BuildPendingWork();

  // Reset speed tracking
  lastSpeedCalcTime_ = std::chrono::steady_clock::now();
  lastProgressCallbackTime_ = std::chrono::steady_clock::now();
  nBytesFromLastSpeedCalc_ = 0;
  downloadSpeed_ = 0;
  eta_ = 0;

  start_download_(this);

  return true;
}

bool DownloadJob::Resume() {
  if (!SetState(DownloadState::Queued)) return false;

  // Wait for any dangling connections to close
  while (nConnections_.load() != 0) {
    std::this_thread::yield();
  }

  BuildPendingWork();

  // Reset speed tracking
  lastSpeedCalcTime_ = std::chrono::steady_clock::now();
  lastProgressCallbackTime_ = std::chrono::steady_clock::now();
  nBytesFromLastSpeedCalc_ = 0;
  downloadSpeed_ = 0;
  eta_ = 0;

  start_download_(this);

  return true;
}

bool DownloadJob::Pause() {
  if (!SetState(DownloadState::Paused)) return false;

  // Wake up any threads waiting on conditions
  std::lock_guard<std::mutex> wait_lock(wait_mtx_);
  wait_cv_.notify_all();

  return true;
}

bool DownloadJob::Cancel() {
  if (!SetState(DownloadState::Canceled)) return false;

  // Wake up any threads waiting on conditions
  std::lock_guard<std::mutex> wait_lock(wait_mtx_);
  wait_cv_.notify_all();

  // TODO: disk clean up

  return true;
}

void DownloadJob::Fail(ErrorCode code, const std::string& detail,
                       int http_status) {
  if (!SetState(DownloadState::Failed)) return;

  {
    // write to disk the current progress
    this->Store();
  }

  {
    std::lock_guard<std::mutex> error_lock(error_mtx_);
    error_.code = code;
    error_.detail = detail;
    error_.http_status = http_status;
  }

  {
    std::lock_guard<std::mutex> wait_lock(wait_mtx_);
    wait_cv_.notify_all();
  }

  std::function<void(MuldError)> on_error_cb;
  {
    std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
    on_error_cb = callbacks_.on_error;
  }
  if (on_error_cb) {
    on_error_cb(error_);
  }
}


bool DownloadJob::SetState(DownloadState next_state) {
  bool state_changed = false;

  if (next_state == DownloadState::Queued) {
    auto current = state_.load();
    while (current == DownloadState::Initialized ||
           current == DownloadState::Paused ||
           current == DownloadState::Canceled ||
           current == DownloadState::Failed) {
      if (state_.compare_exchange_strong(current, DownloadState::Queued)) {
        state_changed = true;
        break;
      }
    }
  } else if (next_state == DownloadState::Downloading) {
    auto expected = DownloadState::Queued;
    if (state_.compare_exchange_strong(expected, DownloadState::Downloading)) {
      state_changed = true;
    }
  } else if (next_state == DownloadState::Paused) {
    auto current = state_.load();
    while (current == DownloadState::Downloading ||
           current == DownloadState::Queued) {
      if (state_.compare_exchange_strong(current, DownloadState::Paused)) {
        state_changed = true;
        break;
      }
    }
  } else if (next_state == DownloadState::Canceled) {
    auto current = state_.load();
    while (current == DownloadState::Downloading ||
           current == DownloadState::Queued ||
           current == DownloadState::Paused ||
           current == DownloadState::Failed) {
      if (state_.compare_exchange_strong(current, DownloadState::Canceled)) {
        state_changed = true;
        break;
      }
    }
  } else if (next_state == DownloadState::Failed) {
    auto current = state_.load();
    while (current == DownloadState::Downloading ||
           current == DownloadState::Queued) {
      if (state_.compare_exchange_strong(current, DownloadState::Failed)) {
        state_changed = true;
        break;
      }
    }
  } else {
    // FORCE OVERRIDE
    state_.store(next_state);
    state_changed = true;
  }

  if (state_changed) {
    std::function<void(DownloadState)> on_state_change_cb;
    {
      std::lock_guard<std::mutex> cb_lock(callbacks_mtx_);
      on_state_change_cb = callbacks_.on_state_change;
    }
    if (on_state_change_cb) {
      on_state_change_cb(next_state);
    }
    return true;
  }

  return false;
}

DownloadState DownloadJob::GetState() const { return state_.load(); }

void DownloadJob::Store() {
  std::lock_guard<std::mutex> disk_lock(disk_mtx_);
  StoreUnlocked();
}

void DownloadJob::StoreUnlocked() {
  auto chunks = std::vector<ChunkState>();
  for (const auto& c : chunksInfo_) {
    chunks.emplace_back(ChunkState{.start_range = c.startRange_,
                                   .end_range = c.endRange_,
                                   .downloaded = c.GetReceivedSize()});
  }

  JobImage img = {.file_path = writer_->filePath_,
                  .file_size = fileSize_,
                  .max_connections = maxConnections_,
                  .ranged = ranged_,
                  .url = GetUrlString(url_),
                  .etag = etag_,
                  .last_modified = lastModified_,
                  .created_at = createdAt_,
                  .updated_at = GetUnixTimestamp(),
                  .chunks = chunks};
  const std::string image_path = writer_->filePath_ + ".muld";
  const bool updated =
      imageStored_ &&
      UpdateImageChunksOnDisk(image_path, imageIndex_, chunks, img.updated_at);
  if (!updated) {
    imageStored_ = WriteImageToDisk(img, image_path, &imageIndex_);
  }
  updatedAt_ = img.updated_at;

  nReceivedBytesFromLastStore_ = 0;
}

bool DownloadJob::NeedsStore() const {
  return nReceivedBytesFromLastStore_ >= 10 * 1024 * 1024;  // 10 MB
}

}  // namespace muld
