#pragma once
#include <functional>
#include <memory>
#include <string>

namespace muld {

enum class LogLevel { Debug, Info, Warning, Error };
using LogCallback = std::function<void(LogLevel, const std::string&)>;

enum class ErrorCode {
  Success = 0,
  HTTP_ERR,
  DISK_ERR,
  NETWORK_ERR,
  SYSTEM_ERR,
  FetchFileInfoFailed,
  ConnectionRefused,
  DiskWriteFailed,
  MaxRedirectsExceeded,
  NotSupported,
};

constexpr const char* GetErrorPrefix(ErrorCode code) {
  switch (code) {
    case ErrorCode::Success:
      return "Success";
    case ErrorCode::FetchFileInfoFailed:
      return "Cannot fetch file head";
    case ErrorCode::ConnectionRefused:
      return "Connection refused";
    case ErrorCode::DiskWriteFailed:
      return "Cannot write to destination disk";
    case ErrorCode::MaxRedirectsExceeded:
      return "Exceeded maximum HTTP redirects";
    case ErrorCode::NotSupported:
      return "Not Supported Request";
    default:
      return "Unknown error";
  }
}

struct MuldError {
  ErrorCode code = ErrorCode::Success;
  int http_status = 0;
  std::string detail;

  std::string GetFormattedMessage() const {
    if (code == ErrorCode::Success) {
      return "No error";
    }
    if (detail.empty()) {
      return GetErrorPrefix(code);
    }
    return std::string(GetErrorPrefix(code)) + ": " + detail;
  }

  // Helper to check if an error exists
  explicit operator bool() const { return code != ErrorCode::Success; }
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

class DownloadJob;
class DownloadHandler {
 public:
  explicit DownloadHandler(DownloadJob* job);

  DownloadProgress GetProgress() const;
  std::vector<ChunkProgress> GetChunksProgress() const;
  bool IsFinished() const;
  const MuldError& GetError() const;
  void Wait() const;

 private:
  DownloadJob* job_;
};

struct MuldConfig {
  int max_threads = 8;  // maximum threads in thread pool
  LogCallback logger = nullptr;
};

struct MuldRequest {
  const char* url;
  const char* destination;
  int max_connections;
};

class ThreadPool;
class MuldDownloadManager {
 public:
  // constructor
  explicit MuldDownloadManager(const MuldConfig& config);
  ~MuldDownloadManager();

  DownloadHandler Download(const MuldRequest& request);
  void WaitAll();
  void Terminate();

 private:
  std::unique_ptr<ThreadPool> threadpool_;
  std::vector<std::shared_ptr<DownloadJob>> jobs_;
  LogCallback logger_;
};

}  // namespace muld