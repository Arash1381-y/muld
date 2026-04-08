#include <muld/muld.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace muld;

namespace {
constexpr const char* kTestUrl =
    "https://dls6.iran-gamecenter-host.com/DonyayeSerial/series4/tt1305826/"
    "SoftSub/S01/720p.BluRay/Adventure.Time.S01E01.720p.BluRay.x264.Pahe."
    "SoftSub.DonyayeSerial.mkv";

constexpr const char* kOutputFile = "test_attach_callbacks_threadsafe.mkv";

auto test_logger = [](muld::LogLevel level, const std::string& msg) {
  if (level == muld::LogLevel::Error) {
    std::cerr << "[MULD ERROR] " << msg << "\n";
  }
};
}  // namespace

int main() {
  std::filesystem::remove(kOutputFile);
  std::filesystem::remove(std::string(kOutputFile) + ".muld");

  std::atomic<std::size_t> progress_a{0};
  std::atomic<std::size_t> progress_b{0};
  std::atomic<std::size_t> chunk_a{0};
  std::atomic<std::size_t> chunk_b{0};
  std::atomic<bool> keep_switching{true};

  MuldConfig config = {8, test_logger};
  MuldRequest request = {kTestUrl, kOutputFile, 8};
  MuldDownloadManager manager(config);

  DownloadCallbacks callback_set_a;
  callback_set_a.on_progress = [&](const DownloadProgress&) {
    progress_a.fetch_add(1, std::memory_order_relaxed);
  };
  callback_set_a.on_chunk_progress = [&](const ChunkProgressEvent&) {
    chunk_a.fetch_add(1, std::memory_order_relaxed);
  };

  DownloadCallbacks callback_set_b;
  callback_set_b.on_progress = [&](const DownloadProgress&) {
    progress_b.fetch_add(1, std::memory_order_relaxed);
  };
  callback_set_b.on_chunk_progress = [&](const ChunkProgressEvent&) {
    chunk_b.fetch_add(1, std::memory_order_relaxed);
  };

  auto [err, handler] = manager.Download(request, callback_set_a);
  if (err) {
    std::cerr << "[Test Failed] Download init failed: " << err.detail << "\n";
    return 1;
  }

  std::thread switcher([&]() {
    for (int i = 0; i < 200 && keep_switching.load(std::memory_order_relaxed);
         ++i) {
      if ((i % 2) == 0) {
        handler->AttachHandlerCallbacks(callback_set_b);
      } else {
        handler->AttachHandlerCallbacks(callback_set_a);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(5));
  keep_switching.store(false, std::memory_order_relaxed);
  switcher.join();

  handler->Cancel();
  handler->Wait();

  const std::size_t total_progress =
      progress_a.load(std::memory_order_relaxed) +
      progress_b.load(std::memory_order_relaxed);
  const std::size_t total_chunk =
      chunk_a.load(std::memory_order_relaxed) +
      chunk_b.load(std::memory_order_relaxed);

  if (total_progress == 0) {
    std::cerr << "[Test Failed] No progress callbacks received while switching.\n";
    return 1;
  }

  if (total_chunk == 0) {
    std::cerr << "[Test Failed] No chunk callbacks received while switching.\n";
    return 1;
  }

  std::cout << "[Test Passed] attachCallbacks is safe during concurrent use.\n";
  return 0;
}
