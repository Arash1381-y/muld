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
  float percentage;
  bool is_complete;
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

 private:
  std::weak_ptr<DownloadJob> job_;
};

}  // namespace muld
