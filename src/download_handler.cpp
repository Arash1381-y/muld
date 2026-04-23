#include <muld/muld.h>

#include <functional>
#include <mutex>

#include "download_engine.h"
#include "url.h"

namespace muld {

namespace {
bool IsTerminalState(DownloadState state) {
  return state == DownloadState::Completed || state == DownloadState::Failed ||
         state == DownloadState::Canceled || state == DownloadState::Paused;
}
}  // namespace

struct DownloadHandler::SharedState {
  explicit SharedState(const Url& in_url, std::string in_output,
                       DownloadCallbacks in_callbacks)
      : url(in_url),
        output_path(std::move(in_output)),
        callbacks(std::move(in_callbacks)) {}

  Url url;
  std::string output_path;
  DownloadCallbacks callbacks;

  std::atomic<DownloadState> state{DownloadState::Initialized};
  std::atomic<DownloadState> desired_state{DownloadState::Initialized};
  std::atomic<size_t> desired_speed_limit_bps{0};
  MuldError error;

  std::shared_ptr<DownloadEngine> engine;
  std::mutex mtx;
  std::condition_variable wait_cv;

  DownloadProgress progress{
      .total_bytes = 0,
      .downloaded_bytes = 0,
      .speed_bytes_per_sec = 0,
      .eta_seconds = 0,
      .percentage = 0.0f,
  };
  std::vector<ChunkProgress> chunks;
};

DownloadHandler::DownloadHandler(const Url& url, const std::string& output_path,
                           const DownloadCallbacks& callbacks)
    : shared_(
          std::make_shared<SharedState>(url, std::string(output_path), callbacks)) {}

void DownloadHandler::AttachHandlerCallbacks(const DownloadCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  shared_->callbacks = callbacks;
}

void DownloadHandler::AttachEngine(std::shared_ptr<DownloadEngine> engine) {
  if (!engine) return;

  std::weak_ptr<SharedState> weak_shared = shared_;
  engine->AttachCallbacks(DownloadCallbacks{
      .on_progress =
          [weak_shared](const DownloadProgress& progress) {
            auto shared = weak_shared.lock();
            if (!shared) return;
            std::function<void(const DownloadProgress&)> cb;
            {
              std::lock_guard<std::mutex> lock(shared->mtx);
              shared->progress = progress;
              cb = shared->callbacks.on_progress;
            }
            if (cb) cb(progress);
          },
      .on_chunk_progress =
          [weak_shared](const ChunkProgressEvent& event) {
            auto shared = weak_shared.lock();
            if (!shared) return;
            std::function<void(const ChunkProgressEvent&)> cb;
            {
              std::lock_guard<std::mutex> lock(shared->mtx);
              if (event.chunk_id >= shared->chunks.size()) {
                shared->chunks.resize(event.chunk_id + 1);
              }
              shared->chunks[event.chunk_id] =
                  ChunkProgress{event.downloaded_bytes, event.total_bytes};
              cb = shared->callbacks.on_chunk_progress;
            }
            if (cb) cb(event);
          },
      .on_state_change =
          [weak_shared](DownloadState state) {
            auto shared = weak_shared.lock();
            if (!shared) return;
            std::function<void(DownloadState)> cb;
            {
              std::lock_guard<std::mutex> lock(shared->mtx);
              shared->state.store(state);
              cb = shared->callbacks.on_state_change;
            }
            if (cb) cb(state);
            if (IsTerminalState(state)) shared->wait_cv.notify_all();
          },
      .on_finish =
          [weak_shared]() {
            auto shared = weak_shared.lock();
            if (!shared) return;
            std::function<void()> cb;
            {
              std::lock_guard<std::mutex> lock(shared->mtx);
              shared->state.store(DownloadState::Completed);
              cb = shared->callbacks.on_finish;
            }
            if (cb) cb();
            shared->wait_cv.notify_all();
          },
      .on_error =
          [weak_shared](MuldError error) {
            auto shared = weak_shared.lock();
            if (!shared) return;
            std::function<void(MuldError)> cb;
            {
              std::lock_guard<std::mutex> lock(shared->mtx);
              shared->error = error;
              shared->state.store(DownloadState::Failed);
              cb = shared->callbacks.on_error;
            }
            if (cb) cb(error);
            shared->wait_cv.notify_all();
          },
  });

  DownloadState desired = shared_->state.load();
  desired = shared_->desired_state.load();
  size_t desired_speed_limit_bps = shared_->desired_speed_limit_bps.load();
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    shared_->engine = std::move(engine);
    shared_->engine->SetSpeedLimit(desired_speed_limit_bps);
    shared_->url = shared_->engine->GetUrl();
    shared_->progress.total_bytes = shared_->engine->GetTotalSize();
    shared_->chunks.clear();
    shared_->chunks.reserve(shared_->engine->GetNumChunks());
    for (size_t i = 0; i < shared_->engine->GetNumChunks(); ++i) {
      auto& chunk = shared_->engine->GetChunkInfo(i);
      shared_->chunks.push_back(
          ChunkProgress{chunk.GetReceivedSize(), chunk.GetTotalSize()});
    }
  }

  if (desired == DownloadState::Canceled) {
    shared_->engine->Cancel();
  } else if (desired == DownloadState::Paused) {
    shared_->engine->Pause();
  } else if (desired == DownloadState::Queued) {
    shared_->engine->Resume();
  }
}

void DownloadHandler::FailBeforeEngineStart(ErrorCode code,
                                         const std::string& detail) {
  std::function<void(MuldError)> cb;
  MuldError err = {.code = code, .detail = detail};
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    shared_->error = err;
    shared_->state.store(DownloadState::Failed);
    shared_->desired_state.store(DownloadState::Failed);
    cb = shared_->callbacks.on_error;
  }
  if (cb) cb(err);
  shared_->wait_cv.notify_all();
}

void DownloadHandler::Pause() {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    engine = shared_->engine;
    shared_->desired_state.store(DownloadState::Paused);
    if (!engine) {
      shared_->state.store(DownloadState::Paused);
      return;
    }
  }
  engine->Pause();
}

void DownloadHandler::Resume() {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    engine = shared_->engine;
    shared_->desired_state.store(DownloadState::Queued);
    if (!engine) {
      shared_->state.store(DownloadState::Queued);
      return;
    }
  }
  engine->Resume();
}

void DownloadHandler::Cancel() {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    engine = shared_->engine;
    shared_->desired_state.store(DownloadState::Canceled);
    if (!engine) {
      shared_->state.store(DownloadState::Canceled);
      shared_->wait_cv.notify_all();
      return;
    }
  }
  engine->Cancel();
}

void DownloadHandler::WaitUntilFinished() {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::unique_lock<std::mutex> lock(shared_->mtx);
    if (!shared_->engine && IsTerminalState(shared_->state.load())) return;
    shared_->wait_cv.wait(lock, [this]() {
      return shared_->engine != nullptr || IsTerminalState(shared_->state.load());
    });
    engine = shared_->engine;
  }
  if (engine) {
    engine->WaitUntilFinished();
  }
}

DownloadState DownloadHandler::GetState() const { return shared_->state.load(); }

const Url& DownloadHandler::GetUrl() const { return shared_->url; }

const MuldError& DownloadHandler::GetError() const {
  thread_local MuldError snapshot;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    snapshot = shared_->error;
  }
  return snapshot;
}

size_t DownloadHandler::GetTotalSize() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->progress.total_bytes;
}

size_t DownloadHandler::GetReceivedSize() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->progress.downloaded_bytes;
}

size_t DownloadHandler::GetDownloadSpeed() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->progress.speed_bytes_per_sec;
}

size_t DownloadHandler::GetJobEta() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->progress.eta_seconds;
}

void DownloadHandler::SetSpeedLimit(size_t speed_limit_bps) {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    shared_->desired_speed_limit_bps.store(speed_limit_bps);
    engine = shared_->engine;
  }
  if (engine) {
    engine->SetSpeedLimit(speed_limit_bps);
  }
}

size_t DownloadHandler::GetSpeedLimit() const {
  std::shared_ptr<DownloadEngine> engine;
  {
    std::lock_guard<std::mutex> lock(shared_->mtx);
    engine = shared_->engine;
    if (!engine) {
      return shared_->desired_speed_limit_bps.load();
    }
  }
  return engine->GetSpeedLimit();
}

DownloadProgress DownloadHandler::GetProgress() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->progress;
}

std::vector<ChunkProgress> DownloadHandler::GetChunksProgress() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->chunks;
}

bool DownloadHandler::IsFinished() const {
  return IsTerminalState(shared_->state.load());
}

bool DownloadHandler::HasError() const {
  std::lock_guard<std::mutex> lock(shared_->mtx);
  return shared_->error.code != ErrorCode::Ok;
}

}  // namespace muld
