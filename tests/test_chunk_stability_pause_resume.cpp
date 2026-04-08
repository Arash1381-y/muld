#include <muld/muld.h>

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

constexpr const char* kOutputFile = "test_chunk_stability_pause_resume.mkv";

auto test_logger = [](muld::LogLevel level, const std::string& msg) {
  if (level == muld::LogLevel::Error) {
    std::cerr << "[MULD ERROR] " << msg << "\n";
  }
};
}  // namespace

int main() {
  std::filesystem::remove(kOutputFile);
  std::filesystem::remove(std::string(kOutputFile) + ".muld");

  MuldConfig config = {8, test_logger};
  MuldRequest request = {kTestUrl, kOutputFile, 8};
  MuldDownloadManager manager(config);

  auto [err, handler] = manager.Download(request);
  if (err) {
    std::cerr << "[Test Failed] Download init failed: " << err.detail << "\n";
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  const auto chunk_count_before = handler->GetChunksProgress().size();
  if (chunk_count_before == 0) {
    std::cerr << "[Test Failed] Empty chunk list.\n";
    handler->Cancel();
    return 1;
  }

  for (int cycle = 0; cycle < 3; ++cycle) {
    handler->Pause();

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto chunk_count_paused = handler->GetChunksProgress().size();
    if (chunk_count_paused != chunk_count_before) {
      std::cerr << "[Test Failed] Chunk count changed after pause.\n";
      handler->Cancel();
      return 1;
    }

    handler->Resume();

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto chunk_count_resumed = handler->GetChunksProgress().size();
    if (chunk_count_resumed != chunk_count_before) {
      std::cerr << "[Test Failed] Chunk count changed after resume.\n";
      handler->Cancel();
      return 1;
    }
  }

  handler->Cancel();
  handler->Wait();

  std::cout << "[Test Passed] Chunk mapping remains stable across pause/resume.\n";
  return 0;
}
