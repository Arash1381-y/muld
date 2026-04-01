#pragma once

#include <atomic>
#include <cstddef>

namespace muld {

class ChunkInfo {
 public:
  std::size_t index;
  std::size_t startRange_;
  std::size_t endRange_;

 public:
  ChunkInfo() : index(0), startRange_(0), endRange_(0), received_(0) {}

  ChunkInfo(const ChunkInfo&) = delete;
  ChunkInfo& operator=(const ChunkInfo&) = delete;

  ChunkInfo(ChunkInfo&& other) noexcept
      : index(other.index),
        startRange_(other.startRange_),
        endRange_(other.endRange_),
        received_(other.received_.load(std::memory_order_relaxed)) {}

  ChunkInfo& operator=(ChunkInfo&& other) noexcept {
    if (this != &other) {
      index = other.index;
      startRange_ = other.startRange_;
      endRange_ = other.endRange_;
      received_.store(other.received_.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }
    return *this;
  }

  ~ChunkInfo() = default;

  std::size_t GetTotalSize() const { return endRange_ - startRange_ + 1; }
  std::size_t GetReceivedSize() const {
    return received_.load(std::memory_order_relaxed);
  }

  float GetProgressPercentage() const {
    auto total = GetTotalSize();
    if (total == 0) return 0.0f;
    return static_cast<float>(GetReceivedSize()) / static_cast<float>(total);
  }

  void UpdateReceived(std::size_t amount) {
    received_.fetch_add(amount, std::memory_order_relaxed);
  }

  bool IsFinished() const { return GetReceivedSize() >= GetTotalSize(); }

 private:
  std::atomic<std::size_t> received_;
};

}  // namespace muld
