#include "http_downloader.h"

#include <muld/error.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <iostream>
#include <limits>
#include <variant>

#include "task.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace muld {
namespace HttpDownloader {

namespace {  // Anonymous namespace for private helper functions

// Custom exceptions for clean error routing
struct HttpException : public std::runtime_error {
  int status_code;
  HttpException(int code, const std::string& msg)
      : std::runtime_error(msg), status_code(code) {}
};

struct DiskException : public std::runtime_error {
  DiskException(const std::string& msg) : std::runtime_error(msg) {}
};

http::request<http::empty_body> BuildRequest(const Url& url,
                                             const ChunkInfo& chunk,
                                             bool isRanged) {
  http::request<http::empty_body> req{http::verb::get, url.path, 11};
  req.set(http::field::host, url.host);
  req.set(http::field::user_agent, "muld/0.1");
  if (isRanged) {
    std::string range_val = "bytes=" + std::to_string(chunk.startRange_) + "-" +
                            std::to_string(chunk.endRange_);
    req.set(http::field::range, range_val);
    req.keep_alive(true);
  }
  return req;
}

FileInfo createFileInfo(const http::response<http::empty_body>& resp) {
  FileInfo info;
  info.is_valid = true;
  if (resp.count(http::field::accept_ranges) &&
      resp[http::field::accept_ranges] == "bytes") {
    info.supports_range = true;
  } else {
    info.supports_range = false;
  }
  info.total_size = std::stoull(std::string(resp[http::field::content_length]));

  return info;
}

void ValidateResponse(const http::response_parser<http::buffer_body>& parser,
                      bool isRanged) {
  auto status = parser.get().result();
  int status_int = parser.get().result_int();

  if (isRanged) {
    if (status != http::status::partial_content) {
      if (status == http::status::ok) {
        throw HttpException(
            status_int,
            "Server ignored Range header and want to sent entire file.");
      }
      throw HttpException(status_int, "HTTP Error during chunk download: " +
                                          std::string(parser.get().reason()));
    }
  } else {
    if (status != http::status::ok) {
      throw HttpException(status_int,
                          "Unexpected HTTP status during download: " +
                              std::string(parser.get().reason()));
    }
  }
}

void StreamBodyToDisk(beast::tcp_stream& stream, beast::flat_buffer& buffer,
                      http::response_parser<http::buffer_body>& parser,
                      DownloadJob* job, const ChunkInfo& chunk) {
  constexpr std::size_t BUFFER_SIZE = 32768;
  char body_buffer[BUFFER_SIZE];
  std::size_t current_offset = chunk.startRange_;

  while (!parser.is_done()) {
    parser.get().body().data = body_buffer;
    parser.get().body().size = BUFFER_SIZE;

    beast::error_code ec;
    http::read(stream, buffer, parser, ec);

    if (ec && ec != http::error::need_buffer &&
        ec != http::error::end_of_stream) {
      throw beast::system_error{ec};
    }

    std::size_t bytes_read = BUFFER_SIZE - parser.get().body().size;
    if (bytes_read > 0) {
      size_t bytes_write = 0;
      while (bytes_write < bytes_read) {
        ssize_t bw =
            job->GetWriter().Write(body_buffer + bytes_write,
                                   bytes_read - bytes_write, current_offset);
        if (bw <= 0) {
          throw DiskException("Failed to write chunk data to disk at offset " +
                              std::to_string(current_offset));
        }
        bytes_write += static_cast<size_t>(bw);
        current_offset += static_cast<size_t>(bw);
      }

      // check if job has critical error
      if (job->GetState() != DownloadJob::DownloadState::Downloading) {
        return;
      }
      job->NotifyChunkReceived(chunk.index, bytes_read);
    }
  }
}

}  // end anonymous namespace

FetchResult FetchFileInfo(const Url& url) {
  FetchResult result;
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const dns_result = resolver.resolve(url.host, url.port);
    stream.connect(dns_result);

    http::request<http::empty_body> req{http::verb::head, url.path, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);

    http::read_some(stream, buffer, parser);
    auto res = parser.release();

    if (res.result() == http::status::ok) {
      result.state = FetchResult::State::SUCCESSFUL;
      result.data = createFileInfo(res);

    } else if (res.result() == http::status::moved_permanently ||
               res.result() == http::status::found) {
      result.state = FetchResult::State::REDIRECT;
      if (res.count(http::field::location)) {
        result.data = FetchRedirect{res[http::field::location].to_string()};
      } else {
        result.state = FetchResult::State::FAILED;
        result.data = FetchError{ErrorCode::HTTP_ERR, res.result_int(),
                                 "Redirect response missing Location header"};
      }
    } else {
      result.state = FetchResult::State::FAILED;
      result.data = FetchError{ErrorCode::HTTP_ERR, res.result_int(),
                               "HTTP Error: " + std::string(res.reason())};
    }

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    if (ec && ec != beast::errc::not_connected) throw beast::system_error{ec};

  } catch (beast::system_error const& e) {
    result.state = FetchResult::State::FAILED;
    result.data =
        FetchError{ErrorCode::NETWORK_ERR, 0,
                   std::string("Network failure during fetch: ") + e.what()};
  } catch (std::exception const& e) {
    result.state = FetchResult::State::FAILED;
    result.data =
        FetchError{ErrorCode::SYSTEM_ERR, 0,
                   std::string("System error during fetch: ") + e.what()};
  }

  return result;
}

void DownloadWorker(const Task& task) {
  const auto& job = task.job;
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    const auto& url = job->GetUrl();
    auto const results = resolver.resolve(url.host, url.port);
    stream.connect(results);

    ssize_t index;
    beast::flat_buffer buffer;
    buffer.reserve(8192);

    while ((index = job->GetNextChunkIndex()) != -1) {
      if (!stream.socket().is_open()) {
        stream.connect(results);
      }

      auto& chunk = job->GetChunkInfo(index);

      auto req = BuildRequest(url, chunk, job->IsRanged());
      http::write(stream, req);

      http::response_parser<http::buffer_body> parser;
      parser.body_limit((std::numeric_limits<std::uint64_t>::max)());

      http::read_header(stream, buffer, parser);
      ValidateResponse(parser, job->IsRanged());

      // Abstracted body processing
      StreamBodyToDisk(stream, buffer, parser, job, chunk);

      if (!parser.keep_alive()) {
        stream.close();
      }
    }

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

  } catch (HttpException const& e) {
    job->Fail(ErrorCode::HTTP_ERR, e.status_code, e.what());
  } catch (DiskException const& e) {
    job->Fail(ErrorCode::DISK_ERR, e.what());
  } catch (beast::system_error const& e) {
    job->Fail(ErrorCode::NETWORK_ERR,
              std::string("Network interrupted: ") + e.what());
  } catch (std::exception const& e) {
    job->Fail(ErrorCode::SYSTEM_ERR,
              std::string("Unexpected system error: ") + e.what());
  }
}

}  // namespace HttpDownloader
}  // namespace muld
