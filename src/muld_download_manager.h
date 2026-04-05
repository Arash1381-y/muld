#pragma once
#include <queue>
#include <unordered_map>
#include <unordered_set>
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

  DownloadHandler Load(const std::string& path);

 private:
  void EnqueueTasks(DownloadJob* job, int connections);

 private:
  LogCallback logger_;
  std::unique_ptr<ThreadPool> threadpool_;
  std::vector<std::shared_ptr<DownloadJob>> jobs_;
  std::unordered_map<std::string, std::weak_ptr<DownloadJob>> jobs_index_;
  std::unordered_set<std::string> loaded_images_;
};

}  // namespace muld
