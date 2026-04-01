#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "muld_download_manager.h"

using namespace muld;

std::string FormatBytes(std::size_t bytes) {
  const char* units[] = {"B", "K", "M", "G", "T"};
  int idx = 0;
  double size = static_cast<double>(bytes);
  while (size >= 1024 && idx < 4) {
    size /= 1024;
    ++idx;
  }
  
  std::ostringstream out;
  out << std::fixed << std::setprecision(idx == 0 ? 0 : 1) << size << units[idx];
  return out.str();
}

std::string FormatTime(double seconds) {
  if (seconds < 0 || std::isnan(seconds) || std::isinf(seconds)) {
    return "---";
  }
  
  if (seconds < 60) {
    return std::to_string(static_cast<int>(seconds)) + "s";
  } else if (seconds < 3600) {
    int m = static_cast<int>(seconds) / 60;
    int s = static_cast<int>(seconds) % 60;
    return std::to_string(m) + "m " + std::to_string(s) + "s";
  } else {
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    return std::to_string(h) + "h " + std::to_string(m) + "m";
  }
}

std::string ProgressBar(float percent, int width = 50) {
  int filled = static_cast<int>((percent / 100.0f) * width);
  std::string bar;
  for (int i = 0; i < width; ++i) {
    if (i < filled - 1) {
      bar += "=";
    } else if (i == filled - 1) {
      bar += ">";
    } else {
      bar += " ";
    }
  }
  return bar;
}

int main() {
  MuldConfig config = {8};
  std::string url = 
      "http://dls4.iran-gamecenter-host.com/DonyayeSerial/movies/1994/"
      "tt0111161/Dubbed/"
      "The.Shawshank.Redemption.1994.480p.BluRay.Dubbed.Pahe.DonyayeSerial.mkv";
      
  const MuldRequest request = {url.c_str(), "redemption.mkv", 4};
  MuldDownloadManager manager(config);
  auto handler = manager.Download(request);

  std::size_t last_bytes = 0;
  double smoothed_speed = 0.0;
  const double alpha = 0.3;
  
  auto start = std::chrono::steady_clock::now();
  auto last_tick = start;

  std::cout << "Downloading: redemption.mkv\n";

  while (!handler.IsFinished()) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_tick).count();
    
    auto total = handler.GetProgress();
    auto chunks = handler.GetChunksProgress();
    
    // Calculate total downloaded across all chunks (from their start)
    std::size_t total_chunk_downloaded = 0;
    std::size_t total_chunk_size = 0;
    
    for (const auto& chunk : chunks) {
      total_chunk_downloaded += chunk.downloaded_bytes;
      total_chunk_size += chunk.total_bytes;
    }
    
    double instant_speed = 0.0;
    double eta = 0.0;
    
    if (elapsed > 0) {
      std::size_t delta = total.downloaded_bytes - last_bytes;
      instant_speed = delta / elapsed;
      
      smoothed_speed = (smoothed_speed == 0.0) 
                       ? instant_speed 
                       : (instant_speed * alpha) + (smoothed_speed * (1.0 - alpha));
      
      if (smoothed_speed > 0 && total_chunk_size > total_chunk_downloaded) {
        eta = (total_chunk_size - total_chunk_downloaded) / smoothed_speed;
      }
    }
    
    last_bytes = total.downloaded_bytes;
    last_tick = now;
    
    float chunk_percent = (total_chunk_size > 0) 
                          ? (static_cast<float>(total_chunk_downloaded) / total_chunk_size) * 100.0f 
                          : 0.0f;
    
    // wget-style output
    std::cout << "\r";
    std::cout << std::setw(3) << std::fixed << std::setprecision(0) << chunk_percent << "% ";
    std::cout << "[" << ProgressBar(chunk_percent, 50) << "] ";
    std::cout << std::setw(8) << FormatBytes(total_chunk_downloaded) << "/" 
              << std::setw(8) << FormatBytes(total_chunk_size) << "  ";
    std::cout << std::setw(8) << FormatBytes(static_cast<std::size_t>(smoothed_speed)) << "/s  ";
    std::cout << "eta " << std::setw(8) << (smoothed_speed > 0 ? FormatTime(eta) : "---") << "  ";
    std::cout << std::flush;
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  
  auto end = std::chrono::steady_clock::now();
  double total_time = std::chrono::duration<double>(end - start).count();
  
  auto final_total = handler.GetProgress();
  
  std::cout << "\r100% [";
  std::cout << std::string(50, '=') << "] ";
  std::cout << std::setw(8) << FormatBytes(final_total.total_bytes) << "/" 
            << std::setw(8) << FormatBytes(final_total.total_bytes) << "  ";
  std::cout << "in " << FormatTime(total_time) << "    \n";
  std::cout << "\nDownload complete: redemption.mkv\n";
  
  return 0;
}
