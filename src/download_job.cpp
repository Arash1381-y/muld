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
                         std::function<void(DownloadJob*)> start_download)
    : maxConnections_(max_connections),
      ranged_(ranged),
      url_(url),
      outputPath_(output_path),
      fileSize_(file_size),
      nTotalReceivedBytes_(0),
      nChunks_(n_chunks),
      nReceivedBytesFromLastStore_(0),
      lastRequestedChunk_(0),
      nDownloadedChunks_(0),
      lastSpeedCalcTime_(std::chrono::steady_clock::now()),
      nBytesFromLastSpeedCalc_(0),
      downloadSpeed_(0),  // (bytes / per sec)
      eta_(0),            // download job eta
      nConnections_(0),
      start_download_(start_download) {
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
    chunk.index = i;
    chunk.startRange_ = i * chunk_size_org;

    if (i == nChunks_ - 1) {
      chunk.endRange_ = fileSize_ - 1;
    } else {
      chunk.endRange_ = chunk.startRange_ + chunk_size_org - 1;
    }
  }
}

DownloadJob::DownloadJob(const JobImage& image,
                         std::function<void(DownloadJob*)> start_download)
    : lastSpeedCalcTime_(std::chrono::steady_clock::now()),
      nBytesFromLastSpeedCalc_(0),
      downloadSpeed_(0),  // (bytes / per sec)
      eta_(0),            // download job eta
      nConnections_(0) {
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
  size_t i = 0;
  size_t already_downloaded = 0;
  for (const auto& c : image.chunks) {
    auto& chunk = chunksInfo_.at(i);
    chunk.index = i;
    chunk.startRange_ = c.start_range;
    chunk.endRange_ = c.end_range;
    chunk.UpdateReceived(c.downloaded);
    already_downloaded += c.downloaded;

    i++;
  }

  size_t first_unfinished_index = CleanUpChunks(chunksInfo_);
  nChunks_ = chunksInfo_.size();
  lastRequestedChunk_ = first_unfinished_index;
  nDownloadedChunks_ = first_unfinished_index;
  nTotalReceivedBytes_ = already_downloaded;
  nReceivedBytesFromLastStore_ = already_downloaded;
}

void DownloadJob::SetValidators(const std::string& etag,
                                const std::string& last_modified) {
  etag_ = etag;
  lastModified_ = last_modified;
}

void DownloadJob::NotifyConnectionOpen() { nConnections_.fetch_add(1); }

void DownloadJob::NotifyConnectionClose() { nConnections_.fetch_sub(1); }

// used for redirection
void DownloadJob::SetUrl(const Url& url) { url_ = url; }

const Url& DownloadJob::GetUrl() const { return url_; }

bool DownloadJob::IsRanged() const { return ranged_; }

std::string DownloadJob::GetIdentityKey() const {
  return outputPath_ + "\n" + GetUrlString(url_);
}

ssize_t DownloadJob::GetNextChunkIndex() {
  size_t index = lastRequestedChunk_.fetch_add(1);
  if (index >= nChunks_) return -1;

  return index;
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

void DownloadJob::NotifyChunkReceived(size_t index, size_t bytes) {
  auto& chunk = chunksInfo_.at(index);
  chunk.UpdateReceived(bytes);

  // check the timer and update the speed if needed
  nBytesFromLastSpeedCalc_.fetch_add(bytes);
  nTotalReceivedBytes_.fetch_add(bytes);
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - lastSpeedCalcTime_).count();
  if (dt > 0.5) {
    std::lock_guard<std::mutex> speed_lock(speed_mtx_);
    now = std::chrono::steady_clock::now();
    dt = std::chrono::duration<double>(now - lastSpeedCalcTime_).count();
    if (dt > 0.5) {
      // compute speed and eta
      downloadSpeed_.store(nBytesFromLastSpeedCalc_ / dt);
      eta_.store((fileSize_ - nTotalReceivedBytes_) / downloadSpeed_);

      // reset states
      nBytesFromLastSpeedCalc_ = 0;
      lastSpeedCalcTime_ = now;
    }

    // TODO: maybe we could notify all locks after updating speed and eta?
  }

  {
    std::lock_guard<std::mutex> disk_lock(disk_mtx_);
    // update number of unwritten bytes to image
    nReceivedBytesFromLastStore_ += bytes;

    if (NeedsStore()) {
      Store();
    }
  }

  if (chunk.IsFinished()) {
    if (nDownloadedChunks_.fetch_add(1, std::memory_order_acq_rel) ==
        nChunks_ - 1) {
      // we only notify the wait if the previous state is downloading (yes we
      // may notify a chunk but have a different state)
      auto expected = DownloadState::Downloading;
      if (state_.compare_exchange_strong(expected, DownloadState::Completed)) {
        std::lock_guard<std::mutex> wait_lock(wait_mtx_);
        wait_cv_.notify_all();
      } else if (expected == DownloadState::Paused ||
                 expected == DownloadState::Failed) {
        // Edge case: User paused or a network error occurred on another
        // thread exactly as the final byte was written. Override it to
        // Completed!
        state_.store(DownloadState::Completed);
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

bool DownloadJob::SetState(DownloadState next_state) {
  if (next_state == DownloadState::Downloading) {
    // RESUME DOWNLOADING STATE
    auto current = state_.load();
    while (current == DownloadState::Initialized ||
           current == DownloadState::Paused ||
           current == DownloadState::Canceled ||
           current == DownloadState::Failed) {
      if (state_.compare_exchange_strong(current, DownloadState::Downloading)) {
        while (nConnections_.load() !=
               0) { /* busy wait for connection close after pause or failed */
        }

        // clean up chunks and reset counts
        size_t first_unfinished_index = CleanUpChunks(chunksInfo_);
        nChunks_ = chunksInfo_.size();
        lastRequestedChunk_ = first_unfinished_index;
        nDownloadedChunks_ = first_unfinished_index;

        start_download_(this);
        return true;
      }
    }
    return false;
  } else if (next_state == DownloadState::Paused) {
    // PAUSE DOWNLOADING STATE
    auto expected = DownloadState::Downloading;
    if (state_.compare_exchange_strong(expected, DownloadState::Paused)) {
      // wake any threads waiting for IsFinished()
      std::lock_guard<std::mutex> wait_lock(wait_mtx_);
      wait_cv_.notify_all();
      return true;
    } else {
      return false;
    }
  } else if (next_state == DownloadState::Canceled) {
    // CANCEL JOB
    auto current = state_.load();
    while (current == DownloadState::Downloading ||
           current == DownloadState::Paused ||
           current == DownloadState::Failed) {
      if (state_.compare_exchange_strong(current, DownloadState::Canceled)) {
        std::lock_guard<std::mutex> wait_lock(wait_mtx_);
        wait_cv_.notify_all();
        return true;
      }
    }
    return false;
  } else {
    state_.store(next_state);
    return true;
  }
}

DownloadState DownloadJob::GetState() const { return state_.load(); }

void DownloadJob::Store() {
  // this function is called mainly for 2 reasons:
  // 1) periodic disk update.
  // 2) special events such as failed downloads (the user may like to retry
  // downloading  after the issue is resolved)

  auto chunks = std::vector<ChunkState>();
  for (const auto& c : chunksInfo_) {
    chunks.emplace_back(ChunkState{.start_range = c.startRange_,
                                   .end_range = c.endRange_,
                                   .downloaded = c.GetReceivedSize()});
  }

  // create a job image and write it to disk
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

  // reset bytes read
  nReceivedBytesFromLastStore_ = 0;
}

void DownloadJob::Fail(ErrorCode code, const std::string& detail,
                       int http_status) {
  auto current_state = state_.load();

  // Loop because state might change between load and CAS
  while (current_state == DownloadState::Downloading) {
    if (state_.compare_exchange_strong(current_state, DownloadState::Failed)) {
      std::lock_guard<std::mutex> disk_lock(disk_mtx_);
      this->Store();

      {
        std::lock_guard<std::mutex> error_lock(error_mtx_);
        error_.code = code;
        error_.detail = detail;
        error_.http_status = http_status;
      }

      std::lock_guard<std::mutex> wait_lock(wait_mtx_);
      wait_cv_.notify_all();
      return;  // Success
    }
  }
}

size_t DownloadJob::CleanUpChunks(std::vector<ChunkInfo>& chunks) {
  size_t originalSize = chunks.size();

  for (size_t i = 0; i < originalSize; ++i) {
    size_t receivedSoFar = chunks[i].GetReceivedSize();

    if (!chunks[i].IsFinished() && receivedSoFar > 0) {
      // create the finished sub-chunk
      ChunkInfo finishedPart;
      finishedPart.index = chunks[i].index;
      finishedPart.startRange_ = chunks[i].startRange_;
      finishedPart.endRange_ = chunks[i].startRange_ + receivedSoFar - 1;
      finishedPart.UpdateReceived(receivedSoFar);

      // create the remaining unfinished sub-chunk
      ChunkInfo unfinishedPart;
      unfinishedPart.index = chunks[i].index;
      unfinishedPart.startRange_ = chunks[i].startRange_ + receivedSoFar;
      unfinishedPart.endRange_ = chunks[i].endRange_;

      // modify the existing vector directly
      chunks[i] = std::move(unfinishedPart);
      chunks.push_back(std::move(finishedPart));
    }
  }

  auto unfinished_begin = std::stable_partition(
      chunks.begin(), chunks.end(),
      [](const ChunkInfo& chunk) { return chunk.IsFinished(); });

  for (size_t i = 0; i < chunks.size(); ++i) {
    chunks[i].index = i;
  }

  // just return the index
  return std::distance(chunks.begin(), unfinished_begin);
}

bool DownloadJob::NeedsStore() const {
  // TODO: maybe we can use this function for special events
  return nReceivedBytesFromLastStore_ >= 10 * 1024 * 1024;  // 10 MB
}

}  // namespace muld
