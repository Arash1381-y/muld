#include <muld/muld.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace muld;

std::string FormatBytes(std::size_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int idx = 0;
  double size = static_cast<double>(bytes);
  while (size >= 1024 && idx < 4) {
    size /= 1024;
    ++idx;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << size
      << units[idx];
  return out.str();
}

std::string FormatTime(double seconds) {
  if (seconds < 0 || std::isnan(seconds) || std::isinf(seconds)) {
    return "--:--:--";
  }
  int h = static_cast<int>(seconds) / 3600;
  int m = (static_cast<int>(seconds) % 3600) / 60;
  int s = static_cast<int>(seconds) % 60;

  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << h << ":" << std::setw(2) << m
      << ":" << std::setw(2) << s;
  return out.str();
}

// Maps all chunks onto a single visual bar
template <typename ChunkContainer>
std::string BuildGlobalProgressBar(const ChunkContainer& chunks,
                                   std::size_t total_size, int width = 50) {
  if (total_size == 0) return std::string(width, ' ');

  std::string bar(width, ' ');
  std::size_t current_offset = 0;

  for (const auto& chunk : chunks) {
    std::size_t start_byte = current_offset;
    std::size_t end_byte = start_byte + chunk.downloaded_bytes;

    int start_pos = static_cast<int>(
        (static_cast<double>(start_byte) / total_size) * width);
    int end_pos =
        static_cast<int>((static_cast<double>(end_byte) / total_size) * width);

    if (start_pos > width) start_pos = width;
    if (end_pos > width) end_pos = width;

    for (int i = start_pos; i < end_pos; ++i) {
      bar[i] = '=';
    }

    if (chunk.downloaded_bytes > 0 &&
        chunk.downloaded_bytes < chunk.total_bytes) {
      if (end_pos < width)
        bar[end_pos] = '>';
      else if (width > 0)
        bar[width - 1] = '>';
    }
    current_offset += chunk.total_bytes;
  }
  return bar;
}

// Extract filename from URL (ignoring query parameters)
std::string ExtractFilenameFromUrl(const std::string& url) {
  std::string clean_url = url;
  auto query_pos = clean_url.find('?');
  if (query_pos != std::string::npos) {
    clean_url = clean_url.substr(0, query_pos);
  }
  auto slash_pos = clean_url.find_last_of('/');
  if (slash_pos != std::string::npos && slash_pos != clean_url.length() - 1) {
    return clean_url.substr(slash_pos + 1);
  }
  return "downloaded_file.out";
}

void PrintHelp() {
  std::cout << "Usage: muld [OPTIONS] <URL>\n"
            << "Options:\n"
            << "  -o, --output <file>    Specify output file path (default: "
               "extracted from URL)\n"
            << "  -c, --connections <N>  Number of concurrent connections "
               "(default: 8)\n"
            << "  -h, --help             Show this help message\n";
}

std::string LogLevelToString(muld::LogLevel level) {
  switch (level) {
    case muld::LogLevel::Debug:
      return "\033[90m[DEBUG]\033[0m ";  // Gray
    case muld::LogLevel::Info:
      return "";
    case muld::LogLevel::Warning:
      return "\033[33m[WARN]\033[0m ";  // Yellow
    case muld::LogLevel::Error:
      return "\033[31m[ERROR]\033[0m ";  // Red
    default:
      return "";
  }
}

std::mutex g_console_mutex;
// Define the logger callback
auto cli_logger = [](muld::LogLevel level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_console_mutex);

  // Clear the current 2 lines of the progress bar to prevent corruption
  std::cout << "\033[2K\r";         // Clear current line (Active chunks)
  std::cout << "\033[1A\033[2K\r";  // Move up and clear line (Main bar)

  // Print the log message
  std::cout << LogLevelToString(level) << msg;

  // Print two empty lines so the progress bar has room to draw on its next tick
  std::cout << "\n\n";
  std::cout << std::flush;
};

void on_progress(const DownloadProgress& progress) {



}

// --- Main CLI Application ---
int main(int argc, char* argv[]) {
  std::string url = "";
  std::string output_path = "";
  int connections = 8;  // Default connections

  // Argument Parsing
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintHelp();
      return 0;
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 < argc)
        output_path = argv[++i];
      else {
        std::cerr << "Error: Missing argument for -o/--output\n";
        return 1;
      }
    } else if (arg == "-c" || arg == "--connections") {
      if (i + 1 < argc) {
        try {
          connections = std::stoi(argv[++i]);
        } catch (...) {
          std::cerr << "Error: Invalid number for connections\n";
          return 1;
        }
      } else {
        std::cerr << "Error: Missing argument for -c/--connections\n";
        return 1;
      }
    } else if (arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      PrintHelp();
      return 1;
    } else {
      url = arg;  // Positional argument is the URL
    }
  }

  if (url.empty()) {
    std::cerr << "Error: No URL provided.\n";
    PrintHelp();
    return 1;
  }

  if (output_path.empty()) {
    output_path = ExtractFilenameFromUrl(url);
  }

  MuldConfig config = {connections, cli_logger};
  const MuldRequest request = {url.c_str(), output_path.c_str(), connections};
  MuldDownloadManager manager(config);

  std::cout << "Starting download...\n";
  std::cout << "URL:  " << url << "\n";
  std::cout << "Dest: " << output_path << "\n";
  std::cout << "Conn: " << connections << "\n\n";

  auto [err, handler] = manager.Download(request);
  if (err) {
    cli_logger(LogLevel::Error, err.detail);
    exit(EXIT_FAILURE);
  }

  std::size_t last_bytes = 0;
  double smoothed_speed = 0.0;
  const double alpha = 0.3;

  auto start = std::chrono::steady_clock::now();
  auto last_tick = start;

  while (!handler->IsFinished()) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_tick).count();

    auto total = handler->GetProgress();
    auto chunks = handler->GetChunksProgress();

    std::size_t total_chunk_downloaded = 0;
    std::size_t total_chunk_size = 0;

    std::vector<std::string> active_chunks_info;

    for (size_t i = 0; i < chunks.size(); ++i) {
      const auto& chunk = chunks[i];
      total_chunk_downloaded += chunk.downloaded_bytes;
      total_chunk_size += chunk.total_bytes;

      if (chunk.downloaded_bytes > 0 &&
          chunk.downloaded_bytes < chunk.total_bytes) {
        int pct = static_cast<int>(
            (static_cast<double>(chunk.downloaded_bytes) / chunk.total_bytes) *
            100);
        active_chunks_info.push_back("[" + std::to_string(i) + ": " +
                                     std::to_string(pct) + "%]");
      }
    }

    double instant_speed = 0.0;
    double eta = 0.0;

    if (elapsed > 0) {
      std::size_t delta = total.downloaded_bytes - last_bytes;
      instant_speed = delta / elapsed;
      smoothed_speed =
          (smoothed_speed == 0.0)
              ? instant_speed
              : (instant_speed * alpha) + (smoothed_speed * (1.0 - alpha));

      if (smoothed_speed > 0 && total_chunk_size > total_chunk_downloaded) {
        eta = (total_chunk_size - total_chunk_downloaded) / smoothed_speed;
      }
    }

    last_bytes = total.downloaded_bytes;
    last_tick = now;

    float chunk_percent =
        (total_chunk_size > 0)
            ? (static_cast<float>(total_chunk_downloaded) / total_chunk_size) *
                  100.0f
            : 0.0f;

    // Build the active chunks summary string
    std::string active_str =
        std::to_string(active_chunks_info.size()) + " Active Chunks: ";

    for (const auto& ac : active_chunks_info) active_str += ac + " ";
    if (active_str.length() > 80)
      active_str = active_str.substr(0, 77) +
                   "...";  // Truncate to fit standard terminal

    // Draw UI using ANSI cursor movements (up 2 lines, clear lines)
    std::cout << "\033[2F\033[2K";
    std::cout << "\r" << std::setw(3) << std::fixed << std::setprecision(0)
              << chunk_percent << "% "
              << "[" << BuildGlobalProgressBar(chunks, total_chunk_size, 40)
              << "] " << std::setw(9) << FormatBytes(total_chunk_downloaded)
              << " / " << FormatBytes(total_chunk_size) << " | " << std::setw(9)
              << FormatBytes(static_cast<std::size_t>(smoothed_speed))
              << "/s | "
              << "ETA " << FormatTime(eta) << "\n";

    std::cout << "\033[2K\r\033[90m" << active_str
              << "\033[0m\n";  // \033[90m makes it gray

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (handler->HasError()) {
    std::cout << "\n\033[31mDOWNLOAD FAILED:\033[0m " << err.detail
              << std::endl;
    return 1;
  }

  auto end = std::chrono::steady_clock::now();
  double total_time = std::chrono::duration<double>(end - start).count();
  auto final_total = handler->GetProgress();

  std::cout << "\033[2F\033[2K";
  std::cout << "\r100% [" << std::string(40, '=') << "] " << std::setw(9)
            << FormatBytes(final_total.total_bytes) << " / " << std::setw(9)
            << FormatBytes(final_total.total_bytes) << " | "
            << "Finished in " << FormatTime(total_time) << "\n\033[2K\n";

  std::cout << "\033[32mDownload complete: \033[0m" << output_path << "\n";

  return 0;
}