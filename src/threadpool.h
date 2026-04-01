#pragma once
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "task.h"

namespace muld {

class ThreadPool {
 public:
  explicit ThreadPool(size_t num_threads,
                      std::function<void(const Task&)> worker_func)
      : stop_(false) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
      workers_.emplace_back([this, worker_func] {
        while (true) {
          Task task;
          {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return !tasks_.empty() || stop_; });
            if (stop_) return;
            task = tasks_.front();
            tasks_.pop();
          }
          worker_func(task);
        }
      });
    }
  }

  ThreadPool(const ThreadPool&) = delete;

  ~ThreadPool() {
    Terminate();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void Enqueue(const Task& task) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      tasks_.push(task);
    }
    cv_.notify_one();
  }

  void Terminate() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      stop_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::vector<std::thread> workers_;
  std::queue<Task> tasks_;

  std::mutex mtx_;
  std::condition_variable cv_;
  bool stop_;
};

}  // namespace muld
