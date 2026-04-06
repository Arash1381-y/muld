#include "download_handler.h"

#include <memory>
#include <vector>

namespace muld {

DownloadHandler::DownloadHandler(std::weak_ptr<DownloadJob> job)
    : job_(std::move(job)) {}

void DownloadHandler::Wait() const { job_.lock()->WaitUntilFinished(); }

HandlerResp DownloadHandler::Pause() {
  auto job = job_.lock();
  if (!job) {
    return {
        {.code = ErrorCode::NotInitialized,
         .detail = "Handler references an invalid or expired job"},
    };
  }

  if (!job->SetState(DownloadState::Paused)) {
    return {
        {.code = ErrorCode::InvalidState, .detail = "Invalid state change"},
    };
  }

  return {MuldError()};
}

HandlerResp DownloadHandler::Resume() {
  auto job = job_.lock();
  if (!job) {
    return {
        {.code = ErrorCode::NotInitialized,
         .detail = "Handler references an invalid or expired job"},
    };
  }

  if (!job->SetState(DownloadState::Downloading)) {
    return {MuldError{.code = ErrorCode::InvalidState,
                      .detail = "Invalid state change"}};
  } else {
    return {MuldError()};
  }
}

HandlerResp DownloadHandler::Cancel() {
  auto job = job_.lock();
  if (!job) {
    return {
        {.code = ErrorCode::NotInitialized,
         .detail = "Handler references an invalid or expired job"},
    };
  }

  if (!job->SetState(DownloadState::Canceled)) {
    return {MuldError{.code = ErrorCode::InvalidState,
                      .detail = "Invalid state change"}};
  } else {
    return {MuldError()};
  }
}

bool DownloadHandler::IsFinished() const {
  auto job = job_.lock();
  if (!job) {
    // user should be aware of false return for invalid job
    return false;
  }

  return job->IsFinished();
}

bool DownloadHandler::HasError() const {
  auto job = job_.lock();
  if (!job) {
    // user should be aware of false return for invalid job
    return false;
  }

  return job->GetError().code != ErrorCode::Ok;
}

DownloadProgress DownloadHandler::GetProgress() const {
  auto job = job_.lock();
  DownloadProgress dp;
  dp.total_bytes = job->GetTotalSize();
  dp.downloaded_bytes = job->GetReceivedSize();
  dp.speed_bytes_per_sec = job->GetDownloadSpeed();
  dp.eta_seconds = job->GetJobEta();

  if (dp.total_bytes > 0) {
    dp.percentage = static_cast<float>(dp.downloaded_bytes) /
                    static_cast<float>(dp.total_bytes) * 100;
  }
  return dp;
}

std::vector<ChunkProgress> DownloadHandler::GetChunksProgress() const {
  auto job = job_.lock();
  std::vector<ChunkProgress> chunks_info;
  chunks_info.reserve(job->GetNumChunks());

  for (size_t i = 0; i < job->GetNumChunks(); i++) {
    const auto& chunk_info = job->GetChunkInfo(i);
    chunks_info.push_back(
        {chunk_info.GetReceivedSize(),  // No longer truncated to int
         chunk_info.GetTotalSize()});
  }

  return chunks_info;
}

const MuldError& DownloadHandler::GetError() const {
  return job_.lock()->GetError();
}

}  // namespace muld
