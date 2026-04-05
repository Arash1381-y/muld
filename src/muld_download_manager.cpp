
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

#include "downloader.h"
#include "url.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

constexpr int MAX_REDIRECT_ALLOW = 3;
constexpr size_t MIN_CHUNK_SIZE = 1 * 1024 * 1024;   // 1 MB
constexpr size_t MAX_CHUNK_SIZE = 10 * 1024 * 1024;  // 10 MB

namespace muld {

MuldDownloadManager::~MuldDownloadManager() = default;

MuldDownloadManager::MuldDownloadManager(const MuldConfig& config)
    : logger_(config.logger),
      threadpool_(std::make_unique<ThreadPool>(
          config.max_threads,
          [](const Task& task) { NetDownloader::DownloadWorker(task); })) {}

void MuldDownloadManager::EnqueueTasks(DownloadJob* job, int num_connections) {
  for (int i = 0; i < num_connections; i++) {
    threadpool_->Enqueue({.job = job, .logger = this->logger_});
  }
}

DownloadHandler MuldDownloadManager::Download(const MuldRequest& request) {
  Url parsed_url = ParseUrl(request.url);

  // initialize job
  auto job = std::make_shared<DownloadJob>(parsed_url, request.destination);
  jobs_.push_back(job);

  FetchResult fetch_res;
  for (int redirect_count = 0; redirect_count < MAX_REDIRECT_ALLOW;
       redirect_count++) {
    if (logger_) {
      std::string url_str =
          (redirect_count == 0)
              ? request.url
              : parsed_url.scheme + "://" + parsed_url.host + parsed_url.path;
      logger_(LogLevel::Info,
              "Resolving and connecting to " + parsed_url.host + "...");
    }

    if (parsed_url.scheme == "http" || parsed_url.scheme == "https") {
      fetch_res = NetDownloader::FetchFileInfo(parsed_url);
    } else {
      if (logger_) {
        logger_(LogLevel::Error, parsed_url.scheme + " is not supported!");
      }
      job->Fail(ErrorCode::NotSupported,
                parsed_url.scheme + " is not supported!");

      return DownloadHandler(job.get());
    }

    if (fetch_res.state == FetchResult::State::SUCCESSFUL) {
      if (logger_) {
        logger_(LogLevel::Info,
                "HTTP request sent, awaiting response... 200/206 OK");
      }
      break;
    } else if (fetch_res.state == FetchResult::State::REDIRECT) {
      auto& url = std::get<FetchRedirect>(fetch_res.data).new_url;
      if (logger_) {
        logger_(
            LogLevel::Info,
            "HTTP request sent, awaiting response... 302 Moved Temporarily");
        logger_(LogLevel::Info, "Location: " + url + " [following]");
      }
      parsed_url = ParseUrl(url);
      job->SetUrl(parsed_url);
    } else {
      // Failed state
      auto& err = std::get<FetchError>(fetch_res.data);
      if (logger_) {
        logger_(LogLevel::Error, "HTTP request failed: " + err.message);
      }
      job->Fail(err.error_code, err.message, err.https_status_code);
      return DownloadHandler(job.get());  // Return gracefully failed handler
    }
  }

  if (fetch_res.state == FetchResult::State::REDIRECT) {
    if (logger_) {
      logger_(LogLevel::Error, "Exceeded max redirects (" +
                                   std::to_string(MAX_REDIRECT_ALLOW) + ")");
    }
    job->Fail(ErrorCode::MaxRedirectsExceeded, "Exceeded max redirects");
    return DownloadHandler(job.get());
  }

  // create tasks
  auto& info = std::get<FileInfo>(fetch_res.data);

  if (logger_) {
    std::string length_str = "unspecified";
    if (info.total_size > 0) {
      // Format like wget: 87274743 (83M)
      size_t mb = info.total_size / (1024 * 1024);
      length_str =
          std::to_string(info.total_size) + " (" + std::to_string(mb) + "M)";
    }

    std::string type_str =
        info.supports_range ? "" : " [No Range Support - Single Connection]";
    logger_(LogLevel::Info, "Length: " + length_str + type_str);
    logger_(LogLevel::Info,
            "Saving to: '" + std::string(request.destination) + "'");
  }

  int num_connections = info.supports_range ? request.max_connections : 1;
  int n_chunks = 1;

  if (info.supports_range && info.total_size > 0) {
    size_t ideal_chunk_size = info.total_size / (num_connections * 4);

    size_t actual_chunk_size =
        std::max(MIN_CHUNK_SIZE, std::min(MAX_CHUNK_SIZE, ideal_chunk_size));

    n_chunks = static_cast<int>((info.total_size + actual_chunk_size - 1) /
                               actual_chunk_size);
  }

  job->Init(info.total_size, info.supports_range, n_chunks, [this, num_connections] (DownloadJob* job) {
    this->EnqueueTasks(job, num_connections);
  } );

  // setting state to downloading will automatically create connections
  job->SetState(DownloadJob::DownloadState::Downloading);

  auto handler = DownloadHandler(job.get());
  return handler;
}

void MuldDownloadManager::WaitAll() {
  for (const auto& j : jobs_) {
    j->WaitUntilFinished();
  }
}

void MuldDownloadManager::Terminate() {
  for (const auto& j : jobs_) {
    if (!j->IsFinished()) {
      j->SetState(DownloadJob::DownloadState::Canceled);
    }
  }
}

}  // namespace muld