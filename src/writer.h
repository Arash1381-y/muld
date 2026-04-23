#pragma once

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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
    fd_ = Open(file_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd_ == -1) {
      throw std::system_error(errno, std::generic_category(),
                               "Failed to open file");
    }

    if (total_size > 0) {
      if (Truncate(fd_, total_size) == -1) {
        throw std::system_error(errno, std::generic_category(),
                                 "Failed to open file");
      }
    }
  }

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer() {
    if (fd_ != -1) {
      Close(fd_);
    }
  }

  ssize_t Write(const char* buffer, std::size_t len, std::size_t offset) {
    return PWrite(fd_, buffer, len, offset);
  }

  std::string filePath_;

 private:
#ifdef _WIN32
  static int Open(const char* path, int flags, int mode) {
    return _open(path, flags | _O_BINARY, mode);
  }

  static int Truncate(int fd, std::size_t total_size) {
    return _chsize_s(fd, total_size) == 0 ? 0 : -1;
  }

  static int Close(int fd) { return _close(fd); }

  static ssize_t PWrite(int fd, const char* buffer, std::size_t len,
                        std::size_t offset) {
    if (_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) == -1) {
      return -1;
    }
    return _write(fd, buffer, static_cast<unsigned int>(len));
  }
#else
  static int Open(const char* path, int flags, int mode) {
    return open(path, flags, mode);
  }

  static int Truncate(int fd, std::size_t total_size) {
    return ftruncate(fd, total_size);
  }

  static int Close(int fd) { return close(fd); }

  static ssize_t PWrite(int fd, const char* buffer, std::size_t len,
                        std::size_t offset) {
    return pwrite(fd, buffer, len, offset);
  }
#endif

  int fd_ = -1;
};

}  // namespace muld
