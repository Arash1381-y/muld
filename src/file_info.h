#pragma once
#include <muld/error.h>

#include <cstddef>
#include <string>
#include <variant>

namespace muld {

struct FileInfo {
  std::size_t total_size = 0;
  bool supports_range = false;
  bool is_valid = false;
};

struct FetchError {
  ErrorCode error_code;
  unsigned int https_status_code;
  std::string message;
};

struct FetchRedirect {
  std::string new_url;
};

struct FetchResult {
  enum class State { FAILED, REDIRECT, SUCCESSFUL };
  State state;
  std::variant<FileInfo, FetchError, FetchRedirect> data;
};

}  // namespace muld
