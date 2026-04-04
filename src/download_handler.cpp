#include "download_handler.h"

#include <memory>
#include <vector>

namespace muld {

DownloadHandler::DownloadHandler(DownloadJob* job) : job_(std::move(job)) {}

DownloadProgress DownloadHandler::GetProgress() const {
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

std::vector<ChunkProgress> DownloadHandler::GetChunksProgress() const {
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

bool DownloadHandler::IsFinished() const { return job_->IsFinished(); }

const MuldError& DownloadHandler::GetError() const { return job_->GetError(); }

void DownloadHandler::Wait() const { job_->WaitUntilFinished(); }

}  // namespace muld
