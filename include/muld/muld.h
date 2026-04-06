#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace muld {

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
class DownloadJob;

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

///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

///////////////////////////////////////
////////// muld downloader ////////////
///////////////////////////////////////
class ThreadPool;

struct MuldConfig {
  int max_threads = 8;  // maximum threads in thread pool
  LogCallback logger = nullptr;
};

struct MuldRequest {
  const char* url;
  const char* destination;
  int max_connections;
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
  void EnqueueTasks(DownloadJob* job, int connections);

 private:
  LogCallback logger_;
  std::unique_ptr<ThreadPool> threadpool_;
  std::vector<std::shared_ptr<DownloadJob>> jobs_;
  std::unordered_map<std::string, std::weak_ptr<DownloadJob>> jobs_index_;
  std::unordered_set<std::string> loaded_images_;
};

///////////////////////////////////////
///////////////////////////////////////
///////////////////////////////////////

}  // namespace muld
