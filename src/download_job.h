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

  DownloadJob(const Url& url, const std::string& output_path);

  DownloadJob(const DownloadJob&) = delete;
  DownloadJob& operator=(const DownloadJob&) = delete;
  DownloadJob(DownloadJob&&) = delete;
  DownloadJob& operator=(DownloadJob&&) = delete;

  ~DownloadJob() {}

  void Init(size_t file_size, bool ranged, size_t n_chunks,
            std::function<void(DownloadJob*)> start_download);

  void WaitUntilFinished();
  void Store();
  void Fail(ErrorCode code, const std::string& detail, int http_status = 0);

  void NotifyConnectionOpen();
  void NotifyConnectionClose();
  void NotifyChunkReceived(size_t index, size_t bytes);

  bool SetState(DownloadState state);
  // used for redirection
  void SetUrl(const Url& url);

  const Url& GetUrl() const;
  const MuldError& GetError() const;
  size_t GetNumChunks() const;
  DownloadState GetState() const;

  ssize_t GetNextChunkIndex();
  ChunkInfo& GetChunkInfo(size_t index);

  Writer& GetWriter();
  const Writer& GetWriter() const;

  bool IsInitialized() const;
  bool IsFinished() const;
  bool IsRanged() const;
  bool NeedsStore() const;

 private:
  size_t CleanUpChunks();

  std::atomic<DownloadState> state_ = DownloadState::Uninitialized;
  bool is_initialized_ = false;
  bool ranged_;
  MuldError error_;

  Url url_;
  std::string outputPath_;
  size_t fileSize_;

  std::unique_ptr<Writer> writer_;
  std::function<void(DownloadJob*)> start_download_;

  size_t nChunks_;
  size_t chunkSize_;
  size_t nReceivedBytes_;  // number of bytes received from last store
  std::atomic<size_t> lastRequestedChunk_;
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;

  std::atomic<int> nConnections_;  // active connections (threads)
  std::mutex wait_mtx_, error_mtx_, disk_mtx_;
  std::condition_variable wait_cv_;
};

}  // namespace muld
