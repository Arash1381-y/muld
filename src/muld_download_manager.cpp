
#include "muld_download_manager.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "http_downloader.h"
#include "url.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

#define MAX_CHUNK_SIZE (35 * (1 << 20))  // (20MB) // TODO: fix this later

namespace muld {

MuldDownloadManager::MuldDownloadManager(const MuldConfig& config)
    : threadpool_(std::make_unique<ThreadPool>(
          config.max_threads, [](const Task& task) {
            // TODO: will add this later
            HttpDownloader::DownloadWorker(task);
          })) {}

DownloadHandler MuldDownloadManager::Download(const MuldRequest& request) {
  Url parsed_url = ParseUrl(request.url);

  FileInfo info;
  if (parsed_url.scheme == "https") {
    // info = HttpsDownloader::FetchFileInfo(parsed_url);
    throw std::runtime_error("HTTPS downloader not yet implemented!");
  } else if (parsed_url.scheme == "http") {
    info = HttpDownloader::FetchFileInfo(parsed_url);
  } else {
    throw std::invalid_argument("Unsupported protocol scheme: " +
                                parsed_url.scheme);
  }

  if (!info.is_valid) {
    throw std::runtime_error("Failed to fetch file headers or invalid URL.");
  }

  int num_connections = info.supports_range ? request.max_connections : 1;
  int nChunks = 1;

  auto job = std::make_shared<DownloadJob>(
      parsed_url, info.total_size, info.supports_range, request.destination);
  jobs_.push_back(job);

  if (info.supports_range) {
    nChunks = static_cast<int>((info.total_size + MAX_CHUNK_SIZE - 1) /
                               MAX_CHUNK_SIZE);

    job->InitChunks(nChunks);

    for (int i = 0; i < request.max_connections; i++) {
      threadpool_->Enqueue({job.get()});
    }

  } else {
    job->InitChunks(nChunks);
    threadpool_->Enqueue({job.get()});
  }

  auto handler = DownloadHandler(job.get());
  return handler;
}

}  // namespace muld