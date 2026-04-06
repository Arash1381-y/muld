#pragma once

#include <memory>
#include <vector>

#include "download_job.h"

namespace muld {

struct HandlerResp {
  MuldError error;

  bool ok() const { return error.code == ErrorCode::Ok; }
  operator bool() const { return ok(); }
};

struct DownloadProgress {
  std::size_t total_bytes;
  std::size_t downloaded_bytes;
  std::size_t speed_bytes_per_sec;
  std::size_t eta_seconds;
  float percentage;
};

struct ChunkProgress {
  std::size_t downloaded_bytes;
  std::size_t total_bytes;
};

class DownloadHandler {
 public:
  explicit DownloadHandler(std::weak_ptr<DownloadJob> job);

  DownloadProgress GetProgress() const;
  std::vector<ChunkProgress> GetChunksProgress() const;
  bool IsFinished() const;
  bool HasError() const;
  const MuldError& GetError() const;
  void Wait() const;
  HandlerResp Pause();
  HandlerResp Resume();
  HandlerResp Cancel();

 private:
  std::weak_ptr<DownloadJob> job_;
};

}  // namespace muld
