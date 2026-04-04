#pragma once
#include <queue>
#include <string>

#include "download_handler.h"
#include "task.h"
#include "threadpool.h"

namespace muld {

struct MuldConfig {
  int max_threads = 8;  // maximum threads in thread pool
  LogCallback logger = nullptr;
};

struct MuldRequest {
  const char* url;
  const char* destination;
  int max_connections;
};

class MuldDownloadManager {
 public:
  // constructor
  explicit MuldDownloadManager(const MuldConfig& config);
  ~MuldDownloadManager();
  DownloadHandler Download(const MuldRequest& request);
  void WaitAll();
  void Terminate();

 private:
  LogCallback logger_;
  std::unique_ptr<ThreadPool> threadpool_;
  std::vector<std::shared_ptr<DownloadJob>> jobs_;
};

}  // namespace muld