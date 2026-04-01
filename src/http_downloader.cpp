#include "http_downloader.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <iostream>
#include <limits>

#include "task.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace muld {
namespace HttpDownloader {

namespace {  // Anonymous namespace for private helper functions

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

void ValidateResponse(const http::response_parser<http::buffer_body>& parser,
                      bool isRanged) {
  auto status = parser.get().result();
  if (isRanged) {
    if (status != http::status::partial_content) {
      if (status == http::status::ok) {
        throw std::runtime_error(
            "Server ignored Range header and sent entire file.");
      }
      throw std::runtime_error("HTTP Error: " +
                               std::to_string(parser.get().result_int()));
    }
  } else {
    if (status != http::status::ok) {
      throw std::runtime_error("Unexpected HTTP status: " +
                               std::to_string(parser.get().result_int()));
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
          throw std::runtime_error("Failed to write to disk");
        }
        bytes_write += static_cast<size_t>(bw);
        current_offset += static_cast<size_t>(bw);
      }

      job->NotifyChunkReceived(chunk.index, bytes_read);
    }
  }
}

}  // end anonymous namespace

FileInfo FetchFileInfo(const Url& url) {
  FileInfo info;
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(url.host, url.port);
    stream.connect(results);

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
      info.is_valid = true;
      if (res.count(http::field::content_length)) {
        info.total_size =
            std::stoull(std::string(res[http::field::content_length]));
      }
      if (res.count(http::field::accept_ranges) &&
          res[http::field::accept_ranges] == "bytes") {
        info.supports_range = true;
      }
    } else {
      std::cerr << "Server returned HTTP error code: " << res.result_int()
                << std::endl;
    }

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    if (ec && ec != beast::errc::not_connected) throw beast::system_error{ec};

  } catch (std::exception const& e) {
    std::cerr << "HttpDownloader Error: " << e.what() << std::endl;
    info.is_valid = false;
  }

  return info;
}

void DownloadWorker(const Task& task) {
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    const auto& url = task.job->GetUrl();
    const auto& job = task.job;
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

  } catch (std::exception const& e) {
    std::cerr << "Download Worker Exception: " << e.what() << "\n";
    // TODO: set error in job later
  }
}

}  // namespace HttpDownloader
}  // namespace muld
