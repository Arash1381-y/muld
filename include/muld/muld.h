#pragma once
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace muld {

struct Url {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;
};

///////////////////////////////////////
//////////////// error ////////////////
///////////////////////////////////////

enum class LogLevel { Debug, Info, Warning, Error };
using LogCallback = std::function<void(LogLevel, const std::string&)>;

enum class ErrorCode {
  Ok = 0,
  NotInitialized,
  InvalidState,
  InvalidRequest,
  HttpError,
  DiskError,
  NetworkError,
  SystemError,
  DuplicateJob,
  FetchFileInfoFailed,
  ConnectionRefused,
  DiskWriteFailed,
  MaxRedirectsExceeded,
  NotSupported,
};

constexpr const char* GetErrorPrefix(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return "Ok";

    case ErrorCode::NotInitialized:
      return "Component not initialized";

    case ErrorCode::InvalidState:
      return "Invalid state";

    case ErrorCode::InvalidRequest:
      return "Invalid request";

    case ErrorCode::HttpError:
      return "HTTP error";

    case ErrorCode::DiskError:
      return "Disk error";

    case ErrorCode::NetworkError:
      return "Network error";

    case ErrorCode::SystemError:
      return "System error";

    case ErrorCode::DuplicateJob:
      return "Duplicate job";

    case ErrorCode::FetchFileInfoFailed:
      return "Cannot fetch file head";

    case ErrorCode::ConnectionRefused:
      return "Connection refused";

    case ErrorCode::DiskWriteFailed:
      return "Cannot write to destination disk";

    case ErrorCode::MaxRedirectsExceeded:
      return "Exceeded maximum HTTP redirects";

    case ErrorCode::NotSupported:
      return "Not supported request";

    default:
      return "Unknown error";
  }
}

struct MuldError {
  ErrorCode code = ErrorCode::Ok;
  int http_status = 0;
  std::string detail;

  std::string GetFormattedMessage() const {
    if (code == ErrorCode::Ok) {
      return "No error";
    }
    if (detail.empty()) {
      return GetErrorPrefix(code);
    }
    return std::string(GetErrorPrefix(code)) + ": " + detail;
  }

  // Helper to check if an error exists
  explicit operator bool() const { return code != ErrorCode::Ok; }
};
///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

///////////////////////////////////////
//// DOWNLOAD QUERY & CALLBACKS////////
///////////////////////////////////////

enum class DownloadState {
  Initialized,
  Queued,
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

struct ChunkProgressEvent {
  std::size_t chunk_id;
  std::size_t downloaded_bytes;
  std::size_t total_bytes;
  bool finished;
};

struct DownloadCallbacks {
  std::function<void(const DownloadProgress&)> on_progress;
  std::function<void(const ChunkProgressEvent&)> on_chunk_progress;
  std::function<void(DownloadState)> on_state_change;
  std::function<void()> on_finish;
  std::function<void(MuldError)> on_error;
};
///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

///////////////////////////////////////
////////////// handler ////////////////
///////////////////////////////////////
class DownloadEngine;

struct ChunkProgress {
  std::size_t downloaded_bytes;
  std::size_t total_bytes;
};

class DownloadHandler {
 public:
  DownloadHandler(const Url& url, const std::string& output_path,
               const DownloadCallbacks& callbacks = {});
  ~DownloadHandler() = default;

  void AttachEngine(std::shared_ptr<DownloadEngine> engine);
  void FailBeforeEngineStart(ErrorCode code, const std::string& detail);

  void AttachHandlerCallbacks(const DownloadCallbacks& callbacks);

  void Pause();
  void Resume();
  void Cancel();
  void WaitUntilFinished();
  void Wait() { WaitUntilFinished(); }

  DownloadState GetState() const;
  const Url& GetUrl() const;

  const MuldError& GetError() const;
  size_t GetTotalSize() const;
  size_t GetReceivedSize() const;
  size_t GetDownloadSpeed() const;
  size_t GetJobEta() const;
  void SetSpeedLimit(size_t speed_limit_bps);
  size_t GetSpeedLimit() const;
  DownloadProgress GetProgress() const;
  std::vector<ChunkProgress> GetChunksProgress() const;
  bool IsFinished() const;
  bool HasError() const;

 private:
  struct SharedState;
  std::shared_ptr<SharedState> shared_;
};

///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

///////////////////////////////////////
////////// muld downloader ////////////
///////////////////////////////////////
class ThreadPool;
class ConnectionController;

struct MuldConfig {
  int max_threads = 8;  // maximum threads in thread pool
  LogCallback logger = nullptr;
};

struct MuldRequest {
  const char* url;
  const char* destination;
  std::size_t speed_limit_bps = 0;  // 0 means unlimited
};

struct DownloaderResp {
  MuldError error;
  std::optional<DownloadHandler> handler;

  bool ok() const { return error.code == ErrorCode::Ok; }
  operator bool() const { return ok(); }
};

class MuldDownloadManager {
 public:
  // constructor
  explicit MuldDownloadManager(const MuldConfig& config);
  ~MuldDownloadManager();

  DownloaderResp Load(const std::string& path,
                      const DownloadCallbacks& callbacks = {});
  DownloaderResp Download(const MuldRequest& request,
                          const DownloadCallbacks& callbacks = {});

  void WaitAll();
  void Terminate();

 private:
  struct PendingDownloadRequest {
    enum class Kind { Download, Load };
    Kind kind;
    Url url;
    std::string destination;
    std::size_t speed_limit_bps = 0;
    std::string image_path;
    DownloadCallbacks callbacks;
    DownloadHandler handler;
  };

  void DownloadDispatcherLoop();
  void ConnectionControlLoop();
  void EnqueueTasks(DownloadEngine* job);

 private:
  LogCallback logger_;
  int max_threads_ = 1;
  std::mutex jobs_mtx_;
  std::vector<std::shared_ptr<DownloadEngine>> jobs_;
  std::unordered_map<std::string, std::weak_ptr<DownloadEngine>> jobs_index_;
  std::unordered_set<std::string> loaded_images_;
  std::unique_ptr<ThreadPool> threadpool_;
  std::unique_ptr<ConnectionController> connection_controller_;
  std::thread dispatcher_thread_;
  std::thread controller_thread_;
  std::queue<PendingDownloadRequest> pending_requests_;
  std::mutex pending_mtx_;
  std::condition_variable pending_cv_;
  std::mutex controller_mtx_;
  std::condition_variable controller_cv_;
  bool stop_dispatcher_ = false;
  bool stop_controller_ = false;
  std::vector<DownloadHandler> handler_;
};

///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

}  // namespace muld
