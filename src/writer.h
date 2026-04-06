#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "error.h"

namespace muld {

class Writer {
 public:
  explicit Writer(const std::string& file_path, size_t total_size)
      : filePath_(file_path) {
    // TODO: writer should be throw free!
    fd_ = open(file_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd_ == -1) {
      throw std::system_error(errno, std::generic_category(),
                               "Failed to open file");
    }

    if (total_size > 0) {
      if (ftruncate(fd_, total_size) == -1) {
        throw std::system_error(errno, std::generic_category(),
                                 "Failed to open file");
      }
    }
  }

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

  ssize_t Write(const char* buffer, std::size_t len, std::size_t offset) {
    return pwrite(fd_, buffer, len, offset);
  }

  std::string filePath_;

 private:
  int fd_ = -1;
};

}  // namespace muld