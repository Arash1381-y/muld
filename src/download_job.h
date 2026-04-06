#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
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

enum class DownloadState {
  Initialized,
  Downloading,
  Completed,
  Failed,
  Paused,
  Canceled
};

struct DownloadProgress {
  std::size_t total_bytes;
  std::size_t downloaded_bytes;
  std::size_t speed_bytes_per_sec;
  std::size_t eta_seconds;
  float percentage;
};

struct DownloadCallbacks {
  std::function<void(const DownloadProgress&)> on_progress;
  std::function<void(DownloadState)> on_state_change;
  std::function<void()> on_finish;
  std::function<void(MuldError)> on_error;
};

class DownloadJob {
 public:
  DownloadJob(const Url& url, const std::string& output_path,
              int max_connections, size_t file_size, bool ranged,
              size_t n_chunks, std::function<void(DownloadJob*)> start_download,
              const DownloadCallbacks& callbacks = {});

  DownloadJob(const JobImage& image,
              std::function<void(DownloadJob*)> start_download,
              const DownloadCallbacks& callbacks = {});

  DownloadJob(const DownloadJob&) = delete;
  DownloadJob& operator=(const DownloadJob&) = delete;
  DownloadJob(DownloadJob&&) = delete;
  DownloadJob& operator=(DownloadJob&&) = delete;

  ~DownloadJob() {}

  void WaitUntilFinished();
  void Store();

  bool Start();
  bool Pause();
  bool Resume();
  bool Cancel();
  void Fail(ErrorCode code, const std::string& detail, int http_status = 0);
  void SetValidators(const std::string& etag, const std::string& last_modified);
  void AttachCallbacks(const DownloadCallbacks& callbacks);

  void NotifyConnectionOpen();
  void NotifyConnectionClose();
  void NotifyChunkReceived(size_t index, size_t bytes);

  // used for redirection
  void SetUrl(const Url& url);

  const Url& GetUrl() const;
  const MuldError& GetError() const;

  size_t GetTotalSize() const;
  size_t GetReceivedSize() const;
  size_t GetDownloadSpeed() const;
  size_t GetJobEta() const;

  size_t GetNumChunks() const;
  DownloadState GetState() const;

  ssize_t GetNextChunkIndex();
  ChunkInfo& GetChunkInfo(size_t index);

  Writer& GetWriter();
  const Writer& GetWriter() const;

  bool IsFinished() const;
  bool IsRanged() const;
  std::string GetIdentityKey() const;

 public:
  int maxConnections_;  // active connections (threads)

 private:
  bool SetState(DownloadState state);

  bool NeedsStore() const;
  size_t CleanUpChunks(std::vector<ChunkInfo>& chunks);

  std::atomic<DownloadState> state_;
  bool ranged_;
  MuldError error_;

  Url url_;
  std::string outputPath_;
  size_t fileSize_;
  std::atomic<size_t> nTotalReceivedBytes_;

  std::unique_ptr<Writer> writer_;
  std::string etag_;
  std::string lastModified_;
  std::uint64_t createdAt_ = 0;
  std::uint64_t updatedAt_ = 0;
  JobImageIndex imageIndex_;
  bool imageStored_ = false;

  size_t nChunks_;
  size_t
      nReceivedBytesFromLastStore_;  // number of bytes received from last store
  std::atomic<size_t> lastRequestedChunk_;
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;

  std::chrono::steady_clock::time_point lastSpeedCalcTime_;
  std::atomic<size_t> nBytesFromLastSpeedCalc_;  // periodic num bytes counter
                                                 // for calculating speed
  std::atomic<double> downloadSpeed_;            // (bytes / per sec)
  std::atomic<double> eta_;                      // download job eta

  std::atomic<int> nConnections_;  // active connections (threads)
  std::mutex wait_mtx_, error_mtx_, disk_mtx_, speed_mtx_;
  std::condition_variable wait_cv_;

  std::function<void(DownloadJob*)> start_download_;

  DownloadCallbacks callbacks_;
};

}  // namespace muld
