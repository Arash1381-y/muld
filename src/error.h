#pragma once
#include <string>

namespace muld {

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

}  // namespace muld
