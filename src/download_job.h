#pragma once

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
  DownloadJob(const Url& url, size_t file_size, bool ranged,
              const std::string& output_path)
      : url_(url),
        ranged_(ranged),
        fileSize_(file_size),
        writer_(std::make_unique<Writer>(output_path, file_size)),
        lastRequestedChunk_(0),
        nDownloadedChunks_(0){};

  DownloadJob(const DownloadJob&) = delete;
  DownloadJob& operator=(const DownloadJob&) = delete;

  DownloadJob(DownloadJob&&) = delete;
  DownloadJob& operator=(DownloadJob&&) = delete;

  ~DownloadJob() {}

  void InitChunks(size_t nChunks) {
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

  int GetError() const { return error_; }

  void NotifyChunkReceived(int index, size_t bytes) {
    auto& chunk = chunksInfo_.at(index);
    chunk.UpdateReceived(bytes);
    if (chunk.IsFinished()) {
      if (nDownloadedChunks_.fetch_add(1, std::memory_order_acq_rel) ==
          nChunks_ - 1) {
        std::lock_guard<std::mutex> lock(mtx_);
        cv_.notify_all();
      }
    }
  }

  bool IsFinished() { return nDownloadedChunks_.load() == nChunks_; }

  void WaitUntilFinished() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]() { return IsFinished(); });
  }

 private:
  Url url_;
  bool ranged_;
  size_t fileSize_;

  std::unique_ptr<Writer> writer_;

  size_t nChunks_;
  std::atomic<size_t> lastRequestedChunk_;
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;
  size_t chunkSize_;

  int error_ = 0;

  std::mutex mtx_;
  std::condition_variable cv_;
};

}  // namespace muld
