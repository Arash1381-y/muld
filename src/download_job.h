#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "chunk_info.h"
#include "job_image.h"
#include "url.h"
#include "writer.h"

namespace muld {

// WorkItem: ephemeral scheduling unit, rebuilt on Start/Resume.
// Workers consume these instead of chunk indices.
struct WorkItem {
  std::size_t work_id;
  std::size_t chunk_id;     // index into chunks_ vector (stable back-reference)
  std::size_t range_start;  // file offset to start downloading from
  std::size_t range_end;    // file offset end (inclusive)
};

class DownloadEngine {
 public:
  DownloadEngine(const Url& url, const std::string& output_path,
                 int max_connections, size_t file_size, bool ranged,
                 size_t n_chunks,
                 std::function<void(DownloadEngine*)> start_download,
                 const DownloadCallbacks& callbacks = {});

  DownloadEngine(const JobImage& image,
              std::function<void(DownloadEngine*)> start_download,
              const DownloadCallbacks& callbacks = {});

  DownloadEngine(const DownloadEngine&) = delete;
  DownloadEngine& operator=(const DownloadEngine&) = delete;
  DownloadEngine(DownloadEngine&&) = delete;
  DownloadEngine& operator=(DownloadEngine&&) = delete;

  ~DownloadEngine() {}

  void WaitUntilFinished();
  void Store();
  bool Start();
  bool Pause();
  bool Resume();
  bool Cancel();

  void Fail(ErrorCode code, const std::string& detail, int http_status = 0);
  void SetValidators(const std::string& etag, const std::string& last_modified);
  void AttachCallbacks(const DownloadCallbacks& callbacks);

  void NotifyConnectionOpen();
  void NotifyConnectionClose();
  void NotifyChunkReceived(size_t chunk_id, size_t bytes);

  // used for redirection
  void SetUrl(const Url& url);

  const Url& GetUrl() const;
  const MuldError& GetError() const;

  size_t GetTotalSize() const;
  size_t GetReceivedSize() const;
  size_t GetDownloadSpeed() const;
  size_t GetJobEta() const;
  void SetSpeedLimit(size_t speed_limit_bps);
  size_t GetSpeedLimit() const;
  size_t AcquireReadBudget(size_t requested_bytes);
  double ConsumeTokenWaitRatio(std::chrono::milliseconds window);

  size_t GetNumChunks() const;
  DownloadState GetState() const;

  WorkItem* GetNextWorkItem();
  ChunkInfo& GetChunkInfo(size_t index);

  Writer& GetWriter();
  const Writer& GetWriter() const;

  bool IsFinished() const;
  bool IsRanged() const;
  std::string GetIdentityKey() const;

  void SetConnectionBounds(int min_connections, int max_connections);
  int GetMinConnections() const;
  int GetMaxConnections() const;
  void SetDesiredConnections(int desired_connections);
  int GetDesiredConnections() const;
  int GetActiveConnections() const;
  int GetScheduledConnections() const;
  int PlanNewWorkers(int max_new_workers);
  bool ShouldReleaseConnection();

 private:
  bool SetState(DownloadState state);
  void StoreUnlocked();
  void CleanupArtifacts(bool remove_output_file);

  bool NeedsStore() const;
  void BuildPendingWork();
  void RefillRateTokens(std::chrono::steady_clock::time_point now);

  std::atomic<DownloadState> state_;
  bool ranged_;
  MuldError error_;

  Url url_;
  std::string outputPath_;
  size_t fileSize_;
  std::atomic<size_t> nTotalReceivedBytes_;

  std::unique_ptr<Writer> writer_;
  std::string etag_;
  std::string lastModified_;
  std::uint64_t createdAt_ = 0;
  std::uint64_t updatedAt_ = 0;
  JobImageIndex imageIndex_;
  bool imageStored_ = false;
  bool artifactsCleaned_ = false;

  size_t nChunks_;
  size_t
      nReceivedBytesFromLastStore_;  // number of bytes received from last store
  std::atomic<size_t> nDownloadedChunks_;
  std::vector<ChunkInfo> chunksInfo_;

  // Work items: ephemeral scheduling units rebuilt on each Start/Resume
  std::vector<WorkItem> pendingWork_;
  std::atomic<size_t> nextWorkItem_;

  std::chrono::steady_clock::time_point lastSpeedCalcTime_;
  std::chrono::steady_clock::time_point lastProgressCallbackTime_;
  std::atomic<size_t> nBytesFromLastSpeedCalc_;  // periodic num bytes counter
                                                 // for calculating speed
  std::atomic<double> downloadSpeed_;            // (bytes / per sec)
  std::atomic<double> eta_;                      // download job eta
  double etaSmoothedSpeedBps_ = 0.0;

  std::atomic<int> nConnections_;  // active connections (threads)
  std::atomic<int> scheduledConnections_{0};
  std::atomic<int> minConnections_{1};
  std::atomic<int> maxConnections_{1};
  std::atomic<int> desiredConnections_{1};
  std::atomic<int> releaseBudget_{0};
  std::mutex wait_mtx_, error_mtx_, disk_mtx_, speed_mtx_, callbacks_mtx_;
  std::condition_variable wait_cv_;

  std::atomic<size_t> speedLimitBps_{0};
  double rateTokens_ = 0.0;
  std::chrono::steady_clock::time_point rateLastRefill_{
      std::chrono::steady_clock::now()};
  std::atomic<std::uint64_t> rateWaitNsWindow_{0};
  std::atomic<std::uint64_t> rateAcquireSamplesWindow_{0};
  mutable std::mutex rate_mtx_;
  std::condition_variable rate_cv_;

  std::function<void(DownloadEngine*)> start_download_;

  DownloadCallbacks callbacks_;
};

}  // namespace muld
