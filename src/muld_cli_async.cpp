#include <muld/muld.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <unordered_map>

using namespace std;
using namespace muld;

void PrintHelp() {
  cout << "Usage: muld [OPTIONS] <URL>\n"
       << "  -o, --output <f>    Output file path (default: extracted)\n"
       << "  -c, --threads <N>   Worker thread pool size (default: 8)\n"
       << "  -s, --speed <RATE>  Speed limit, e.g. 20MB/s, 20M, 20K (default: "
          "unlimited)\n"
       << "  -h, --help          Show this help message\n";
}

bool ParseSpeedLimit(const string& input, size_t& out_bps) {
  string s;
  s.reserve(input.size());
  for (char ch : input) {
    if (!isspace(static_cast<unsigned char>(ch))) {
      s.push_back(static_cast<char>(toupper(static_cast<unsigned char>(ch))));
    }
  }
  if (s.empty()) return false;

  size_t i = 0;
  bool dot_seen = false;
  while (i < s.size()) {
    char ch = s[i];
    if (isdigit(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    if (ch == '.' && !dot_seen) {
      dot_seen = true;
      ++i;
      continue;
    }
    break;
  }

  if (i == 0) return false;

  double value = 0.0;
  try {
    value = stod(s.substr(0, i));
  } catch (...) {
    return false;
  }
  if (!isfinite(value) || value < 0.0) return false;

  string unit = s.substr(i);
  if (unit.size() >= 2 && unit.compare(unit.size() - 2, 2, "/S") == 0) {
    unit.erase(unit.size() - 2);
  }
  if (unit == "PS") unit.clear();
  if (unit == "BPS") unit = "B";

  size_t multiplier = 1;
  if (unit.empty() || unit == "B") {
    multiplier = 1;
  } else if (unit == "K" || unit == "KB" || unit == "KI" || unit == "KIB") {
    multiplier = 1024ULL;
  } else if (unit == "M" || unit == "MB" || unit == "MI" || unit == "MIB") {
    multiplier = 1024ULL * 1024ULL;
  } else if (unit == "G" || unit == "GB" || unit == "GI" || unit == "GIB") {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else if (unit == "T" || unit == "TB" || unit == "TI" || unit == "TIB") {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else {
    return false;
  }

  long double scaled = static_cast<long double>(value) * multiplier;
  if (scaled < 0.0L ||
      scaled > static_cast<long double>(numeric_limits<size_t>::max())) {
    return false;
  }

  out_bps = static_cast<size_t>(scaled);
  return true;
}

bool parseArgs(int argc, char** argv, string& url, string& out,
               int& max_threads, size_t& speed_limit_bps) {
  for (int i = 1; i < argc; ++i) {
    string a = argv[i];
    if (a == "-h" || a == "--help") return PrintHelp(), false;
    if (a == "-o" || a == "--output") {
      if (++i < argc)
        out = argv[i];
      else
        return cerr << "Missing -o arg\n", false;
    } else if (a == "-c" || a == "--threads" || a == "--conn") {
      if (++i < argc)
        max_threads = stoi(argv[i]);
      else
        return cerr << "Missing -c arg\n", false;
    } else if (a == "-s" || a == "--speed") {
      if (++i < argc) {
        if (!ParseSpeedLimit(argv[i], speed_limit_bps)) {
          return cerr << "Invalid speed format: " << argv[i] << "\n", false;
        }
      } else {
        return cerr << "Missing -s arg\n", false;
      }
    } else if (a[0] == '-')
      return cerr << "Unknown option: " << a << "\n", false;
    else
      url = a;
  }
  return true;
}

string ExtractFilenameFromUrl(string url) {
  if (auto p = url.find('?'); p != string::npos) url.erase(p);
  auto p = url.find_last_of('/');
  return (p != string::npos && p != url.length() - 1) ? url.substr(p + 1)
                                                      : "download.out";
}

string FormatBytes(size_t bytes) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double b = bytes;
  for (; b >= 1024 && i < 4; ++i) b /= 1024;
  char buf[32];
  snprintf(buf, 32, i ? "%.2f%s" : "%.0f%s", b, u[i]);
  return buf;
}

string FormatTime(double sec) {
  if (sec < 0 || isnan(sec) || isinf(sec)) return "--:--:--";
  int s = sec;
  char buf[32];
  snprintf(buf, 32, "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
  return buf;
}

string BuildProgressBar(const vector<ChunkProgress>& chunks, size_t tot,
                        int w = 40) {
  if (!tot) return string(w, ' ');
  string bar(w, ' ');
  size_t off = 0;
  for (const auto& c : chunks) {
    int s = min(w, (int)((off * w) / tot)),
        e = min(w, (int)(((off + c.downloaded_bytes) * w) / tot));
    for (int i = s; i < e; ++i) bar[i] = '=';
    if (c.downloaded_bytes > 0 && c.downloaded_bytes < c.total_bytes)
      bar[e < w ? e : w - 1] = '>';
    off += c.total_bytes;
  }
  return bar;
}

mutex g_mtx;
auto cli_logger = [](LogLevel l, const string& msg) {
  lock_guard<mutex> lock(g_mtx);
  string pfx = l == LogLevel::Debug     ? "\033[90m[DEBUG]\033[0m "
               : l == LogLevel::Warning ? "\033[33m[WARN]\033[0m "
               : l == LogLevel::Error   ? "\033[31m[ERROR]\033[0m "
                                        : "";
  cout << "\033[2K\r\033[1A\033[2K\r" << pfx << msg << "\n\n" << flush;
};

int main(int argc, char* argv[]) {
  string url, out;
  int max_threads = 8;
  size_t speed_limit_bps = 0;
  if (!parseArgs(argc, argv, url, out, max_threads, speed_limit_bps)) return 1;
  if (url.empty()) return cerr << "Error: No URL provided.\n", PrintHelp(), 1;
  if (out.empty()) out = ExtractFilenameFromUrl(url);

  bool c_mod = false, d_mod = false;
  size_t average_bandwidth;
  DownloadProgress d_prog;
  vector<ChunkProgress> all;
  unordered_map<int, ChunkProgress> active;

  auto on_chunk = [&](const ChunkProgressEvent& e) {
    if (e.chunk_id >= all.size()) all.resize(e.chunk_id + 1);
    all[e.chunk_id] = {e.downloaded_bytes, e.total_bytes};
    if (!e.finished)
      active[e.chunk_id] = all[e.chunk_id];
    else
      active.erase(e.chunk_id);
    c_mod = true;
  };

  MuldDownloadManager mgr({max_threads, cli_logger});
  cout << "Starting download...\nURL:  " << url << "\nDest: " << out
       << "\nThreads: " << max_threads << "\nSpeed: "
       << (speed_limit_bps == 0 ? "unlimited"
                                : std::to_string(speed_limit_bps) + " B/s")
       << "\n\n";

  auto start = std::chrono::steady_clock::now();
  auto [err, task] =
      mgr.Download({url.c_str(), out.c_str(), speed_limit_bps},
                   {.on_progress =
                        [&](const auto& p) {
                          d_prog = p;
                          d_mod = true;
                        },
                    .on_chunk_progress = on_chunk,
                    .on_error =
                        [&](MuldError e) {
                          cli_logger(LogLevel::Error, e.GetFormattedMessage());
                        }});

  if (err) return cli_logger(LogLevel::Error, err.detail), 1;

  while (!task->IsFinished()) {
    if (!c_mod && !d_mod) continue;
    c_mod = d_mod = false;

    string act = to_string(active.size()) + " Active Chunks: ";
    for (const auto& [id, c] : active)
      act += "[" + to_string(id) + ":" +
             to_string((c.downloaded_bytes * 100) / c.total_bytes) + "%] ";
    if (act.length() > 100) act = act.substr(0, 97) + "...";

    printf(
        "\033[2F\033[2K\r%3.0f%% [%s] %9s / %s | %9s/s | ETA "
        "%s\n\033[2K\r\033[90m%s\033[0m\n",
        d_prog.percentage, BuildProgressBar(all, d_prog.total_bytes).c_str(),
        FormatBytes(d_prog.downloaded_bytes).c_str(),
        FormatBytes(d_prog.total_bytes).c_str(),
        FormatBytes(d_prog.speed_bytes_per_sec).c_str(),
        FormatTime(d_prog.eta_seconds).c_str(), act.c_str());
    fflush(stdout);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (task->HasError()) {
    exit(EXIT_FAILURE);
  }

  exit(EXIT_SUCCESS);
}
