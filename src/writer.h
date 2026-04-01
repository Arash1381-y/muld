#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>

namespace muld {

class Writer {
 public:
  explicit Writer(const std::string& file_path, size_t total_size) {
    fd_ = open(file_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd_ == -1) {
      throw std::system_error(errno, std::generic_category(),
                              "Failed to open file");
    }

    if (total_size > 0) {
      ftruncate(fd_, total_size);
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

 private:
  int fd_ = -1;
};

}  // namespace muld