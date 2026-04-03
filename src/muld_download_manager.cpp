
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

constexpr size_t MAX_CHUNK_SIZE = (35 * (1 << 20));
constexpr int MAX_REDIRECT_ALLOW = 3;

namespace muld {

MuldDownloadManager::MuldDownloadManager(const MuldConfig& config)
    : threadpool_(std::make_unique<ThreadPool>(
          config.max_threads, [](const Task& task) {
            // TODO: will add this later
            HttpDownloader::DownloadWorker(task);
          })) {}

DownloadHandler MuldDownloadManager::Download(const MuldRequest& request) {
  Url parsed_url = ParseUrl(request.url);

  // initialize job
  auto job = std::make_shared<DownloadJob>(parsed_url, request.destination);
  jobs_.push_back(job);

  // fetch file info (header)
  FetchResult fetch_res;
  for (int redirect_count = 0; redirect_count < MAX_REDIRECT_ALLOW;
       redirect_count++) {
    if (parsed_url.scheme == "http") {
      fetch_res = HttpDownloader::FetchFileInfo(parsed_url);
    } else {
      job->Fail(ErrorCode::NotSupported,
                parsed_url.scheme + " is not supported!");

      return DownloadHandler(job.get());
    }

    if (fetch_res.state == FetchResult::State::SUCCESSFUL) {
      break;
    } else if (fetch_res.state == FetchResult::State::REDIRECT) {
      auto& url = std::get<FetchRedirect>(fetch_res.data).new_url;
      parsed_url = ParseUrl(url);

      


    } else {
      // Failed state
      auto& err = std::get<FetchError>(fetch_res.data);
      job->Fail(err.error_code, err.https_status_code, err.message);
      return DownloadHandler(job.get());  // Return gracefully failed handler
    }
  }

  if (fetch_res.state == FetchResult::State::REDIRECT) {
    job->Fail(ErrorCode::MaxRedirectsExceeded, "Exceeded max redirects");
    return DownloadHandler(job.get());
  }

  // create tasks
  auto& info = std::get<FileInfo>(fetch_res.data);

  int num_connections = info.supports_range ? request.max_connections : 1;
  int nChunks = info.supports_range
                    ? static_cast<int>((info.total_size + MAX_CHUNK_SIZE - 1) /
                                       MAX_CHUNK_SIZE)
                    : 1;

  job->SetState(DownloadJob::DownloadState::Downloading);
  job->Init(info.total_size, info.supports_range, nChunks);
  for (int i = 0; i < num_connections; i++) {
    threadpool_->Enqueue({job.get()});
  }

  auto handler = DownloadHandler(job.get());
  return handler;
}

}  // namespace muld