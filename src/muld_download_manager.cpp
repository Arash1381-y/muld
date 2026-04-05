
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
#include <filesystem>
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

namespace {

std::optional<FileInfo> ResolveValidatedFileInfo(const Url& initial_url,
                                                 Url& resolved_url) {
  FetchResult fetch_res;
  resolved_url = initial_url;

  for (int redirect_count = 0; redirect_count < MAX_REDIRECT_ALLOW;
       redirect_count++) {
    fetch_res = NetDownloader::FetchFileInfo(resolved_url);
    if (fetch_res.state == FetchResult::State::SUCCESSFUL) {
      return std::get<FileInfo>(fetch_res.data);
    }
    if (fetch_res.state != FetchResult::State::REDIRECT) {
      return std::nullopt;
    }

    resolved_url = ParseUrl(std::get<FetchRedirect>(fetch_res.data).new_url);
  }

  return std::nullopt;
}

bool MatchesStoredFile(const JobImage& img, const FileInfo& info) {
  if (img.file_size > 0 && info.total_size > 0 && img.file_size != info.total_size) {
    return false;
  }
  if (!img.etag.empty() && !info.etag.empty() && img.etag != info.etag) {
    return false;
  }
  if (!img.last_modified.empty() && !info.last_modified.empty() &&
      img.last_modified != info.last_modified) {
    return false;
  }
  return true;
}

}

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
  auto job = std::make_shared<DownloadJob>(parsed_url, request.destination, request.max_connections);
  jobs_.push_back(job);
  jobs_index_[job->GetIdentityKey()] = job;

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

  job->Init(info.total_size, info.supports_range, n_chunks,
            [this](DownloadJob* job) {
              this->EnqueueTasks(job, job->maxConnections_);
            });
  job->SetValidators(info.etag, info.last_modified);

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
      j->Store();
      j->SetState(DownloadJob::DownloadState::Canceled);
    }
  }
}

DownloadHandler MuldDownloadManager::Load(const std::string& path) {
  auto make_failed_handler = [&](const Url& url, const std::string& output_path,
                                 int max_connections, ErrorCode code,
                                 const std::string& message) {
    auto failed_job =
        std::make_shared<DownloadJob>(url, output_path, max_connections);
    failed_job->Fail(code, message);
    jobs_.push_back(failed_job);
    return DownloadHandler(failed_job.get());
  };

  JobImage img;
  if (!ReadImageFromDisk(img, path)) {
    return make_failed_handler(ParseUrl("http://invalid/"), "", 1,
                               ErrorCode::SYSTEM_ERR,
                               "Failed to read job image from disk");
  }

  if (!loaded_images_.insert(path).second) {
    return make_failed_handler(ParseUrl(img.url), img.file_path,
                               img.max_connections, ErrorCode::SYSTEM_ERR,
                               "This job image is already loaded");
  }

  Url corrected_url;
  auto info = ResolveValidatedFileInfo(ParseUrl(img.url), corrected_url);
  if (!info.has_value()) {
    loaded_images_.erase(path);
    return make_failed_handler(ParseUrl(img.url), img.file_path,
                               img.max_connections, ErrorCode::NETWORK_ERR,
                               "Failed to validate stored job URL");
  }

  if (!MatchesStoredFile(img, *info)) {
    loaded_images_.erase(path);
    return make_failed_handler(corrected_url, img.file_path,
                               img.max_connections, ErrorCode::HTTP_ERR,
                               "Stored job image no longer matches remote file");
  }

  img.url = GetUrlString(corrected_url);
  img.file_size = info->total_size;
  img.ranged = info->supports_range;
  img.max_connections = info->supports_range ? img.max_connections : 1;
  img.etag = info->etag;
  img.last_modified = info->last_modified;

  const std::string identity_key = img.file_path + "\n" + img.url;
  auto existing = jobs_index_.find(identity_key);
  if (existing != jobs_index_.end() && !existing->second.expired()) {
    loaded_images_.erase(path);
    return make_failed_handler(corrected_url, img.file_path,
                               img.max_connections, ErrorCode::SYSTEM_ERR,
                               "An equivalent download job is already loaded");
  }

  if (!std::filesystem::exists(img.file_path)) {
    loaded_images_.erase(path);
    return make_failed_handler(corrected_url, img.file_path,
                               img.max_connections, ErrorCode::DISK_ERR,
                               "Target file is missing on disk");
  }

  auto local_size = std::filesystem::file_size(img.file_path);
  if (img.file_size > 0 && local_size != img.file_size) {
    loaded_images_.erase(path);
    return make_failed_handler(
        corrected_url, img.file_path, img.max_connections, ErrorCode::DISK_ERR,
        "Target file size does not match stored job image");
  }

  auto job = std::make_shared<DownloadJob>(
      img, [this](DownloadJob* job) { this->EnqueueTasks(job, job->maxConnections_); });

  jobs_.push_back(job);
  jobs_index_[job->GetIdentityKey()] = job;
  return DownloadHandler(job.get());
}

}  // namespace muld
