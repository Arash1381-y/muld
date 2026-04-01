#pragma once

#include <memory>
#include <vector>

#include "download_job.h"

namespace muld {

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
  // Use shared_ptr to ensure the job outlives the handler if needed
  explicit DownloadHandler(DownloadJob* job) : job_(std::move(job)) {}

  DownloadProgress GetProgress() const {
    DownloadProgress dp = {0, 0, 0.0f, false};

    for (size_t i = 0; i < job_->GetNumChunks(); i++) {
      const auto& chunk_info = job_->GetChunkInfo(i);
      dp.total_bytes += chunk_info.GetTotalSize();
      dp.downloaded_bytes += chunk_info.GetReceivedSize();
    }

    if (dp.total_bytes > 0) {
      dp.percentage = static_cast<float>(dp.downloaded_bytes) /
                      static_cast<float>(dp.total_bytes) * 100;
    }

    // Rely on the source of truth, not a floating point comparison
    dp.is_complete = job_->IsFinished();

    return dp;
  }

  std::vector<ChunkProgress> GetChunksProgress() const {
    std::vector<ChunkProgress> chunks_info;
    chunks_info.reserve(job_->GetNumChunks());

    for (size_t i = 0; i < job_->GetNumChunks(); i++) {
      const auto& chunk_info = job_->GetChunkInfo(i);
      chunks_info.push_back(
          {chunk_info.GetReceivedSize(),  // No longer truncated to int
           chunk_info.GetTotalSize()});
    }

    return chunks_info;
  }

  bool IsFinished() const { return job_->IsFinished(); }

  void Wait() const { job_->WaitUntilFinished(); }

 private:
  DownloadJob* job_;
};

}  // namespace muld
