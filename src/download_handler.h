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

struct ChunkProgress {
  std::size_t downloaded_bytes;
  std::size_t total_bytes;
};


class DownloadHandler {
 public:
  explicit DownloadHandler(std::weak_ptr<DownloadJob> job);

  void AttachHandlerCallbacks(const DownloadCallbacks& callbacks);

  HandlerResp Pause();
  HandlerResp Resume();
  HandlerResp Cancel();
  void Wait() const;

  bool IsFinished() const;
  bool HasError() const;

  const MuldError& GetError() const;
  DownloadProgress GetProgress() const;
  std::vector<ChunkProgress> GetChunksProgress() const;

 private:
  std::weak_ptr<DownloadJob> job_;
};

}  // namespace muld
