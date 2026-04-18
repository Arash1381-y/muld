#include <muld/muld.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace muld;

auto test_logger = [](muld::LogLevel level, const std::string& msg) {
  if (level == muld::LogLevel::Error) {
    std::cerr << "[MULD ERROR] " << msg << "\n";
  }
};

int main(int argc, char* argv[]) {
  std::string test_url =
      "http://dl2.sermoviedown.pw/Series/2025/Nero.the.Assassin/720p.10bit/"
      "Nero.the.Assassin.S01E01.DUAL-AUDIO.FRE-ENG.720p.10bit.WEBRip.2CH.x265."
      "HEVC-PSA.Sermovie.mkv";
  std::string output_file = "test_nero.mkv";

  if (argc > 1) {
    test_url = argv[1];
  }

  std::filesystem::remove(output_file);
  std::filesystem::remove(output_file + ".muld");

  {
    std::cout << "--- Starting Terminate/Load Integration Test ---\n";
    MuldConfig config = {10, test_logger};
    MuldRequest request = {test_url.c_str(), output_file.c_str()};
    MuldDownloadManager manager(config);

    std::cout << "[Test] Initiating download...\n";
    auto [err, handler] = manager.Download(request);
    if (err) {
      std::cerr << err.detail << std::endl;
      return 1;
    }

    std::cout << "[Test] Downloading for 15 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(15));

    auto progress_before_terminate = handler->GetProgress();
    if (progress_before_terminate.downloaded_bytes == 0) {
      std::cerr << "[Test Failed] No data downloaded before terminate.\n";
      return 1;
    }

    std::cout << "[Test] Terminating manager...\n";
    manager.Terminate();
    std::this_thread::sleep_for(std::chrono::seconds(3));

    if (!std::filesystem::exists(output_file + ".muld")) {
      std::cerr << "[Test Failed] Resume image was not stored.\n";
      return 1;
    }
  }

  {
    MuldConfig config = {10, test_logger};
    MuldDownloadManager manager(config);

    std::cout << "[Test] Loading stored job...\n";
    auto [err, handler] = manager.Load(output_file + ".muld");
    if (err) {
      std::cerr << err.detail << std::endl;
      return 1;
    }

    std::cout << "[Test] Resuming loaded job...\n";
    handler->Resume();

    while (!handler->IsFinished()) {
      auto progress = handler->GetProgress();
      std::cout << "[INFO] " << progress.downloaded_bytes << " / "
                << progress.total_bytes << " | " << progress.percentage << " %"
                << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (handler->HasError()) {
      std::cerr << "[Test Failed] Loaded download finished with an error: "
                << handler->GetError().detail << "\n";
      return 1;
    }

    auto final_progress = handler->GetProgress();
    if (final_progress.downloaded_bytes != final_progress.total_bytes) {
      std::cerr << "[Test Failed] Size mismatch after load/resume! Downloaded: "
                << final_progress.downloaded_bytes << " / "
                << final_progress.total_bytes << "\n";
      return 1;
    }
  }

  std::cout << "[Test Passed] Terminate/load/resume completed successfully.\n";
  return 0;
}
