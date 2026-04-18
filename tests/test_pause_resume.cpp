#include <muld/muld.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace muld;

// A simple test logger to keep track of what the manager is doing
auto test_logger = [](muld::LogLevel level, const std::string& msg) {
  if (level == muld::LogLevel::Error) {
    std::cerr << "[MULD ERROR] " << msg << "\n";
  }
};

int main(int argc, char* argv[]) {
  // We need a decently sized file to ensure we have time to pause it.
  // This is a standard 100MB test file from Hetzner.
  std::string test_url = "http://87.107.105.160:8000/test_pause_resume.bin";
  std::string output_file = "test_pause_resume.bin";

  if (argc > 1) {
    test_url = argv[1];
  }

  std::cout << "--- Starting Pause/Resume Integration Test ---\n";

  MuldConfig config = {8, test_logger};  // 8 connections
  MuldRequest request = {test_url.c_str(), output_file.c_str()};
  MuldDownloadManager manager(config);

  // 1. Start Download
  std::cout << "[Test] Initiating download...\n";
  auto [err, handler] = manager.Download(request);

  if (err) {
    std::cerr << err.detail << std::endl;
    return 1;
  }

  // 2. Let it download for a short time
  std::cout << "[Test] Downloading for 2 seconds...\n";
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 3. Trigger Pause
  std::cout << "[Test] Attempting to Pause...\n";
  handler->Pause();
  std::cout << "[Test] Pause signaled successfully.\n";

  // Wait a moment for worker threads to finish their current in-flight buffers
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  auto progress_after_pause = handler->GetProgress();
  std::size_t bytes_at_pause = progress_after_pause.downloaded_bytes;
  std::cout << "[Test] Bytes at pause: " << bytes_at_pause << "\n";

  std::cout
      << "[Test] Waiting 3 seconds to ensure progress does not increase...\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));

  auto progress_during_pause = handler->GetProgress();
  std::size_t bytes_during_pause = progress_during_pause.downloaded_bytes;
  std::cout << "[Test] Bytes after waiting: " << bytes_during_pause << "\n";

  if (bytes_at_pause != bytes_during_pause) {
    std::cerr << "[Test Failed] Download did NOT stop! Progress increased by "
              << (bytes_during_pause - bytes_at_pause)
              << " bytes while paused.\n";
    return 1;
  }
  std::cout << "[Test Passed] Download completely stopped while paused.\n";

  // 5. Trigger Resume
  std::cout << "[Test] Attempting to Resume...\n";
  handler->Resume();
  std::cout << "[Test] Resume signaled successfully.\n";

  // 6. Verify that downloading continues
  std::this_thread::sleep_for(std::chrono::seconds(2));
  auto progress_after_resume = handler->GetProgress();

  if (progress_after_resume.downloaded_bytes <= bytes_during_pause) {
    std::cerr << "[Test Failed] Download did NOT resume! Progress is stuck.\n";
    return 1;
  }
  std::cout << "[Test Passed] Download successfully resumed.\n";

  // 7. Wait for Completion
  std::cout << "[Test] Waiting for download to finish...\n";
  while (!handler->IsFinished()) {
    auto progress = handler->GetProgress();
    std::cout << "[INFO] " << progress.downloaded_bytes << " / "
              << progress.total_bytes << " | " << progress.percentage << " %"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  if (handler->HasError()) {
    std::cerr << "[Test Failed] Download finished with an error: "
              << handler->GetError().detail << "\n";
    return 1;
  }

  auto final_progress = handler->GetProgress();
  if (final_progress.downloaded_bytes != final_progress.total_bytes) {
    std::cerr << "[Test Failed] Size mismatch! Downloaded: "
              << final_progress.downloaded_bytes << " / "
              << final_progress.total_bytes << "\n";
    return 1;
  }

  std::cout << "[Test Passed] Download completed successfully. Final size: "
            << final_progress.total_bytes << " bytes.\n";
  std::cout << "--- All Tests Passed! ---\n";

  return 0;
}
