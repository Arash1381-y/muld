#pragma once

#include <muld/error.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "chunk_info.h"
#include "url.h"
#include "writer.h"

namespace muld {

class DownloadJob {
 public:
  enum class DownloadState {
    Initialized,
    Downloading,
    Completed,
    Failed,
    Paused,
    Canceled
  };

  DownloadJob(const Url& url, const std::string& output_path)
      : url_(url),
        outputPath_(output_path),
        lastRequestedChunk_(0),
        nDownloadedChunks_(0) {
    state_ = DownloadState::Initialized;
  };

  void Init(size_t file_size, bool ranged, size_t nChunks) {
    fileSize_ = file_size;
    ranged_ = ranged;
    writer_ = std::make_unique<Writer>(outputPath_, fileSize_);
    nChunks_ = nChunks;
    chunksInfo_.resize(nChunks);
    chunkSize_ = static_cast<size_t>((fileSize_ + nChunks - 1) / nChunks);
    for (size_t i = 0; i < nChunks; i++) {
      auto& chunk = chunksInfo_.at(i);
      chunk.index = i;
      chunk.startRange_ = i * chunkSize_;

      auto chunk_size = chunkSize_;
      if (i == nChunks - 1) {
        chunk_size -= chunkSize_ * nChunks_ - fileSize_;
      }
      chunk.endRange_ = chunk.startRange_ + chunk_size - 1;
    }
  }

  DownloadJob(const DownloadJob&) = delete;

  DownloadJob& operator=(const DownloadJob&) = delete;

  DownloadJob(DownloadJob&&) = delete;
  DownloadJob& operator=(DownloadJob&&) = delete;

  ~DownloadJob() {}

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

  const size_t GetNumChunks() const { return nChunks_; }

  const MuldError& GetError() const { return error_; }

  void NotifyChunkReceived(int index, size_t bytes) {
    auto& chunk = chunksInfo_.at(index);
    chunk.UpdateReceived(bytes);
    if (chunk.IsFinished()) {
      if (nDownloadedChunks_.fetch_add(1, std::memory_order_acq_rel) ==
          nChunks_ - 1) {
        std::lock_guard<std::mutex> lock(wait_mtx_);
        state_ = DownloadState::Completed;
        wait_cv_.notify_all();
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

  void SetState(DownloadState state) { state_ = state; }
  DownloadState GetState() { return state_.load(); }

  // internal fails
  void Fail(ErrorCode code, const std::string& detail) {
    std::lock_guard<std::mutex> erro_lock(error_mtx_);
    if (state_ == DownloadState::Failed) return;

    state_ = DownloadState::Failed;
    error_.code = code;
    error_.detail = detail;
    error_.http_status = 0;

    std::lock_guard<std::mutex> wait_lock(wait_mtx_);
    wait_cv_.notify_all();
  }

  // network fails
  void Fail(ErrorCode code, int http_status, const std::string& detail) {
    std::lock_guard<std::mutex> erro_lock(error_mtx_);
    if (state_ == DownloadState::Failed) return;

    state_ = DownloadState::Failed;
    error_.code = code;
    error_.detail = detail;
    error_.http_status = http_status;

    std::lock_guard<std::mutex> wait_lock(wait_mtx_);
    wait_cv_.notify_all();
  }

 private:
  Url url_;
  bool ranged_;
  size_t fileSize_;

  std::string outputPath_;
  std::unique_ptr<Writer> writer_;

  size_t nChunks_;
  std::atomic<size_t> lastRequestedChunk_;
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;
  size_t chunkSize_;

  std::atomic<DownloadState> state_ = DownloadState::Initialized;
  MuldError error_;

  std::mutex wait_mtx_, error_mtx_;
  std::condition_variable wait_cv_;
};

}  // namespace muld
