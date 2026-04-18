#include <muld/muld.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

using namespace muld;

namespace {
constexpr const char* kTestUrl =
    "https://dls6.iran-gamecenter-host.com/DonyayeSerial/series4/tt1305826/"
    "SoftSub/S01/720p.BluRay/Adventure.Time.S01E01.720p.BluRay.x264.Pahe."
    "SoftSub.DonyayeSerial.mkv";

constexpr const char* kOutputFile = "test_callbacks_progress.mkv";

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::seconds timeout,
               std::chrono::milliseconds poll = std::chrono::milliseconds(100)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(poll);
  }
  return predicate();
}

auto test_logger = [](muld::LogLevel level, const std::string& msg) {
  if (level == muld::LogLevel::Error) {
    std::cerr << "[MULD ERROR] " << msg << "\n";
  }
};
}  // namespace

int main() {
  std::filesystem::remove(kOutputFile);
  std::filesystem::remove(std::string(kOutputFile) + ".muld");

  std::atomic<std::size_t> progress_calls{0};
  std::atomic<std::size_t> chunk_calls{0};
  std::atomic<bool> invalid_chunk_id{false};
  std::atomic<std::size_t> expected_chunk_count{0};
  std::mutex seen_mutex;
  std::unordered_set<std::size_t> seen_chunk_ids;

  MuldConfig config = {8, test_logger};
  MuldRequest request = {kTestUrl, kOutputFile};
  MuldDownloadManager manager(config);

  auto [err, handler] = manager.Download(request, DownloadCallbacks{
                                                      .on_progress = [&](const DownloadProgress&) {
                                                        progress_calls.fetch_add(1, std::memory_order_relaxed);
                                                      },
                                                      .on_chunk_progress =
                                                          [&](const ChunkProgressEvent& chunk_event) {
                                                            chunk_calls.fetch_add(1, std::memory_order_relaxed);
                                                            const auto chunk_count =
                                                                expected_chunk_count.load(std::memory_order_relaxed);
                                                            if (chunk_count > 0 && chunk_event.chunk_id >= chunk_count) {
                                                              invalid_chunk_id.store(true, std::memory_order_relaxed);
                                                            }
                                                            std::lock_guard<std::mutex> lock(seen_mutex);
                                                            seen_chunk_ids.insert(chunk_event.chunk_id);
                                                          },
                                                  });

  if (err) {
    std::cerr << "[Test Failed] Download init failed: " << err.detail << "\n";
    return 1;
  }
  expected_chunk_count.store(handler->GetChunksProgress().size(),
                             std::memory_order_relaxed);

  if (!WaitUntil([&]() { return chunk_calls.load() > 5; }, std::chrono::seconds(12))) {
    std::cerr << "[Test Failed] on_chunk_progress was not called enough times.\n";
    handler->Cancel();
    return 1;
  }

  if (!WaitUntil([&]() { return progress_calls.load() > 0; }, std::chrono::seconds(8))) {
    std::cerr << "[Test Failed] on_progress was not called periodically.\n";
    handler->Cancel();
    return 1;
  }

  if (invalid_chunk_id.load(std::memory_order_relaxed)) {
    std::cerr << "[Test Failed] on_chunk_progress emitted invalid chunk_id.\n";
    handler->Cancel();
    return 1;
  }

  handler->Pause();

  std::this_thread::sleep_for(std::chrono::seconds(2));
  handler->Resume();

  const auto chunk_count_after_resume = handler->GetChunksProgress().size();
  std::size_t max_seen_chunk = 0;
  {
    std::lock_guard<std::mutex> lock(seen_mutex);
    for (const auto chunk_id : seen_chunk_ids) {
      if (chunk_id > max_seen_chunk) {
        max_seen_chunk = chunk_id;
      }
    }
  }

  if (!seen_chunk_ids.empty() && max_seen_chunk >= chunk_count_after_resume) {
    std::cerr << "[Test Failed] Chunk IDs changed after resume.\n";
    handler->Cancel();
    return 1;
  }

  handler->Cancel();
  handler->Wait();

  std::cout << "[Test Passed] on_progress and on_chunk_progress callbacks work.\n";
  return 0;
}
