#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "chunk_info.h"
#include "error.h"
#include "job_image.h"
#include "url.h"
#include "writer.h"

namespace muld {

class DownloadJob {
 public:
  enum class DownloadState {
    Uninitialized,
    Initialized,
    Downloading,
    Completed,
    Failed,
    Paused,
    Canceled
  };

  DownloadJob(const Url& url, const std::string& output_path)
      : url_(url),
        nReceivedBytes_(0),
        outputPath_(output_path),
        nConnections_(0),
        lastRequestedChunk_(0),
        nDownloadedChunks_(0) {
    state_ = DownloadState::Uninitialized;
  };

  void Init(size_t file_size, bool ranged, size_t n_chunks,
            std::function<void(DownloadJob*)> start_download) {
    is_initialized_ = true;
    fileSize_ = file_size;
    ranged_ = ranged;
    writer_ = std::make_unique<Writer>(outputPath_, fileSize_);
    nChunks_ = n_chunks;
    chunksInfo_.resize(n_chunks);
    start_download_ = start_download;
    chunkSize_ = static_cast<size_t>((fileSize_ + nChunks_ - 1) / nChunks_);
    for (size_t i = 0; i < nChunks_; i++) {
      auto& chunk = chunksInfo_.at(i);
      chunk.index = i;
      chunk.startRange_ = i * chunkSize_;

      auto chunk_size = chunkSize_;
      if (i == nChunks_ - 1) {
        chunk_size -= chunkSize_ * nChunks_ - fileSize_;
      }
      chunk.endRange_ = chunk.startRange_ + chunk_size - 1;
    }
    this->SetState(DownloadState::Initialized);
  }

  DownloadJob(const DownloadJob&) = delete;

  DownloadJob& operator=(const DownloadJob&) = delete;

  DownloadJob(DownloadJob&&) = delete;
  DownloadJob& operator=(DownloadJob&&) = delete;

  ~DownloadJob() {}

  bool IsInitialized() const { return is_initialized_; }

  void NotifyConnectionOpen() { nConnections_.fetch_add(1); }

  void NotifyConnectionClose() { nConnections_.fetch_sub(1); }

  // used for redirection
  void SetUrl(const Url& url) { url_ = url; }

  const Url& GetUrl() const { return url_; }

  bool IsRanged() const { return ranged_; }

  ssize_t GetNextChunkIndex() {
    size_t index = lastRequestedChunk_.fetch_add(1);
    if (index >= nChunks_) return -1;

    return index;
  }

  ChunkInfo& GetChunkInfo(size_t index) {
    if (index >= nChunks_) {
      throw std::runtime_error("Chunk index is invalid");
    }

    return chunksInfo_.at(index);
  }

  Writer& GetWriter() { return *writer_; }
  const Writer& GetWriter() const { return *writer_; }

  size_t GetNumChunks() const { return nChunks_; }

  const MuldError& GetError() const { return error_; }

  void NotifyChunkReceived(int index, size_t bytes) {
    auto& chunk = chunksInfo_.at(index);
    chunk.UpdateReceived(bytes);

    {
      std::lock_guard<std::mutex> disk_lock(disk_mtx_);
      // update number of unwritten bytes to image
      nReceivedBytes_ += bytes;

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
        if (state_.compare_exchange_strong(expected,
                                           DownloadState::Completed)) {
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

  bool IsFinished() {
    return nDownloadedChunks_.load() == nChunks_ ||
           state_.load() == DownloadState::Canceled ||
           state_.load() == DownloadState::Paused ||
           state_.load() == DownloadState::Completed ||
           state_.load() == DownloadState::Failed;
  }

  void WaitUntilFinished() {
    std::unique_lock<std::mutex> lock(wait_mtx_);
    wait_cv_.wait(lock, [this]() { return IsFinished(); });
  }

  bool SetState(DownloadState state) {
    if (state == DownloadState::Downloading) {
      auto current = state_.load();
      while (current == DownloadJob::DownloadState::Initialized ||
             current == DownloadJob::DownloadState::Paused ||
             current == DownloadJob::DownloadState::Failed) {
        if (state_.compare_exchange_strong(current,
                                           DownloadState::Downloading)) {
          // busy waite on resume for immediate resume after pause call
          while (nConnections_.load() !=
                 0) { /* busy wait for connection close*/
          }

          // clean up chunks and reset counts
          size_t first_unfinished_index = CleanUpChunks();
          nChunks_ = chunksInfo_.size();
          lastRequestedChunk_ = first_unfinished_index;
          nDownloadedChunks_ = first_unfinished_index;

          start_download_(this);
          return true;
        }
      }
      return false;

    } else if (state == DownloadState::Paused) {
      auto expected = DownloadState::Downloading;
      if (state_.compare_exchange_strong(expected, DownloadState::Paused)) {
        // wake any threads waiting for IsFinished()
        std::lock_guard<std::mutex> wait_lock(wait_mtx_);
        wait_cv_.notify_all();
        return true;
      } else {
        return false;
      }
    } else {
      state_.store(state);
      return true;
    }
  }

  DownloadState GetState() { return state_.load(); }

  bool NeedsStore() {
    // TODO: maybe we can use this function for special events
    return nReceivedBytes_ >= 10 * 1024 * 1024;  // 10 MB
  }

  void Store() {
    // this function is called mainly for 2 reasons:
    // 1) periodic disk update.
    // 2) special events such as failed downloads (the user may like to retry
    // downloading  after the issue is resolved)

    auto chunks = std::vector<UnFinishedChunk>();
    for (const auto& c : chunksInfo_) {
      if (!c.IsFinished()) {
        chunks.emplace_back(
            UnFinishedChunk{.start_range = c.startRange_ + c.GetReceivedSize(),
                            .end_range = c.endRange_});
      }
    }

    // create a job image and write it to disk
    JobImage img = {.file_path = writer_->filePath_,
                    .url = GetUrlString(url_),
                    .chunks = chunks};
    WriteImageToDisk(img, writer_->filePath_ + ".muld");

    // reset bytes read
    nReceivedBytes_ = 0;
  }

  void Fail(ErrorCode code, const std::string& detail, int http_status = 0) {
    auto current_state = state_.load();

    // Loop because state might change between load and CAS
    while (current_state == DownloadState::Downloading ||
           current_state == DownloadState::Uninitialized) {
      if (state_.compare_exchange_strong(current_state,
                                         DownloadState::Failed)) {
        if (is_initialized_) {
          std::lock_guard<std::mutex> disk_lock(disk_mtx_);
          this->Store();
        }

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

 private:
  Url url_;
  bool ranged_;
  size_t fileSize_;
  size_t nReceivedBytes_;  // number of bytes received from last store

  std::string outputPath_;
  std::unique_ptr<Writer> writer_;

  std::atomic<int> nConnections_;

  size_t nChunks_;
  std::atomic<size_t> lastRequestedChunk_;
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;
  size_t chunkSize_;
  std::function<void(DownloadJob*)> start_download_;

  std::atomic<DownloadState> state_ = DownloadState::Uninitialized;
  bool is_initialized_ = false;
  MuldError error_;

  std::mutex wait_mtx_, error_mtx_, disk_mtx_;
  std::condition_variable wait_cv_;

  size_t CleanUpChunks() {
    size_t originalSize = chunksInfo_.size();

    for (size_t i = 0; i < originalSize; ++i) {
      size_t receivedSoFar = chunksInfo_[i].GetReceivedSize();

      if (!chunksInfo_[i].IsFinished() && receivedSoFar > 0) {
        // create the finished sub-chunk representing the completed part
        ChunkInfo finishedPart;
        finishedPart.index =
            chunksInfo_[i].index;  // Inherits the same chunk ID
        finishedPart.startRange_ = chunksInfo_[i].startRange_;
        finishedPart.endRange_ = chunksInfo_[i].startRange_ + receivedSoFar - 1;
        finishedPart.UpdateReceived(
            receivedSoFar);  // This makes IsFinished() return true

        // create the remaining unfinished sub-chunk
        ChunkInfo unfinishedPart;
        unfinishedPart.index = chunksInfo_[i].index;
        unfinishedPart.startRange_ = chunksInfo_[i].startRange_ + receivedSoFar;
        unfinishedPart.endRange_ = chunksInfo_[i].endRange_;
        // received_ naturally defaults to 0

        // replace the current item with the unfinished part
        chunksInfo_[i] = std::move(unfinishedPart);
        chunksInfo_.push_back(std::move(finishedPart));
      }
    }

    auto unfinished_begin = std::stable_partition(
        chunksInfo_.begin(), chunksInfo_.end(),
        [](const ChunkInfo& chunk) { return chunk.IsFinished(); });

    for (size_t i = 0; i < chunksInfo_.size(); ++i) {
      chunksInfo_[i].index = i;
    }

    // the index of the first unfinished chunk
    return std::distance(chunksInfo_.begin(), unfinished_begin);
  }
};

}  // namespace muld
