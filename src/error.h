#pragma once
#include <string>

namespace muld {

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

}  // namespace muld
