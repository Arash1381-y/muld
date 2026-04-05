#include "downloader.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <concepts>
#include <iostream>
#include <limits>
#include <optional>
#include <variant>

#include "task.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace muld {
namespace NetDownloader {

namespace {  // Anonymous namespace for private helper functions

template <typename T>
concept StreamType = std::is_same_v<T, beast::tcp_stream> ||
    std::is_same_v<T, beast::ssl_stream<beast::tcp_stream>>;

struct ConnectionGuard {
  DownloadJob* job;
  explicit ConnectionGuard(DownloadJob* j) : job(j) {
    job->NotifyConnectionOpen();
  }
  ~ConnectionGuard() { job->NotifyConnectionClose(); }

  // Prevent copying
  ConnectionGuard(const ConnectionGuard&) = delete;
  ConnectionGuard& operator=(const ConnectionGuard&) = delete;
};

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
                                             std::size_t current_offset,
                                             bool isRanged) {
  http::request<http::empty_body> req{http::verb::get, url.path, 11};
  req.set(http::field::host, url.host);
  req.set(http::field::user_agent, "muld/0.1");
  if (isRanged) {
    // Request from our current progress point up to the chunk end
    std::string range_val = "bytes=" + std::to_string(current_offset) + "-" +
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
    // TODO: sometimes server support ranges but they do not include range
    // header so maybe we can send a simple 10 byte range and check the response
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

template <StreamType T>
bool StreamBodyToDisk(T& stream, beast::flat_buffer& buffer,
                      http::response_parser<http::buffer_body>& parser,
                      DownloadJob* job, const ChunkInfo& chunk,
                      std::size_t& current_offset) {
  constexpr std::size_t BUFFER_SIZE = 32768;
  char body_buffer[BUFFER_SIZE];

  while (!parser.is_done()) {
    parser.get().body().data = body_buffer;
    parser.get().body().size = BUFFER_SIZE;

    // we only check the state before doing the read. after that
    // no matter what we write on disk and notify the job
    if (job->GetState() != DownloadJob::DownloadState::Downloading) {
      return false;
    }

    beast::error_code ec;
    http::read(stream, buffer, parser, ec);

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

      job->NotifyChunkReceived(chunk.index, bytes_read);
    }

    if (ec) {
      if (ec == http::error::need_buffer) {
        continue;  // Normal behavior for buffer_body
      } else if (ec == http::error::partial_message ||
                 ec == net::ssl::error::stream_truncated ||
                 ec == net::error::eof || ec == net::error::connection_reset) {
        // Interrupted by server. Return false to signal resume.
        return false;
      } else {
        // Fatal system/network error
        throw beast::system_error{ec};
      }
    }
  }
  return true;  // Successfully finished this chunk
}

template <StreamType T>
FetchResult FetchFileInfoImpl(T& stream, const Url& url) {
  FetchResult result;
  try {
    http::request<http::empty_body> req{http::verb::head, url.path, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);

    http::read(stream, buffer, parser);
    auto res = parser.release();

    if (res.result() == http::status::ok) {
      result.state = FetchResult::State::SUCCESSFUL;
      result.data = createFileInfo(res);

    } else if (res.result() == http::status::moved_permanently ||
               res.result() == http::status::found ||
               res.result() == http::status::see_other ||
               res.result() == http::status::temporary_redirect ||
               res.result() == http::status::permanent_redirect) {
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

    beast::get_lowest_layer(stream).socket().shutdown(
        tcp::socket::shutdown_both, ec);
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

template <StreamType T>
void DownloadWorkerImpl(const Task& task,
                        const tcp::resolver::results_type& results,
                        net::ssl::context* ctx = nullptr) {
  const auto& job = task.job;
  const auto& url = job->GetUrl();

  try {
    net::io_context ioc;
    std::optional<T> stream;

    auto connect_stream = [&]() {
      if constexpr (std::is_same_v<T, beast::tcp_stream>) {
        stream.emplace(ioc);
        stream->connect(results);
      } else {
        stream.emplace(ioc, *ctx);
        beast::get_lowest_layer(*stream).connect(results);

        if (!SSL_set_tlsext_host_name(stream->native_handle(),
                                      url.host.c_str())) {
          boost::system::error_code ec{static_cast<int>(::ERR_get_error()),
                                       net::error::get_ssl_category()};
          throw boost::system::system_error{ec};
        }

        stream->handshake(net::ssl::stream_base::client);
      }
    };

    ssize_t index;
    beast::flat_buffer buffer;
    buffer.reserve(8192);

    bool is_connected = false;

    while ((index = job->GetNextChunkIndex()) != -1) {
      auto& chunk = job->GetChunkInfo(index);

      std::size_t current_offset = chunk.startRange_;
      bool chunk_finished = false;

      // TODO: seems we do not give up on a request. is this the correct way?
      // Keep downloading until this chunk is perfectly finished
      while (!chunk_finished) {
        if (job->GetState() != DownloadJob::DownloadState::Downloading) {
          return;
        }

        if (!is_connected) {
          connect_stream();
          is_connected = true;
        }

        auto req = BuildRequest(url, chunk, current_offset, job->IsRanged());
        http::write(*stream, req);

        http::response_parser<http::buffer_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());

        http::read_header(*stream, buffer, parser);
        ValidateResponse(parser, job->IsRanged());

        // StreamBodyToDisk tracks current_offset internally
        chunk_finished = StreamBodyToDisk(*stream, buffer, parser, job, chunk,
                                          current_offset);

        // If interrupted before finished, or server requested close
        if (!chunk_finished || !parser.keep_alive()) {
          if (!chunk_finished && !job->IsRanged()) {
            throw HttpException(0,
                                "Connection interrupted and server does not "
                                "support Range requests.");
          }

          beast::error_code ec;
          if constexpr (std::is_same_v<T,
                                       beast::ssl_stream<beast::tcp_stream>>) {
            stream->shutdown(ec);
          }
          beast::get_lowest_layer(*stream).socket().shutdown(
              tcp::socket::shutdown_both, ec);
          beast::get_lowest_layer(*stream).socket().close(
              ec);  // Ensure closed before emplace

          stream.reset();
          is_connected = false;
        }
      }
    }

    // Task final clean up
    if (is_connected && stream) {
      beast::error_code ec;
      if constexpr (std::is_same_v<T, beast::ssl_stream<beast::tcp_stream>>) {
        stream->shutdown(ec);
      }
      beast::get_lowest_layer(*stream).socket().shutdown(
          tcp::socket::shutdown_both, ec);
    }

  } catch (HttpException const& e) {
    // http error
    job->Fail(ErrorCode::HTTP_ERR, e.what(), e.status_code);
  } catch (DiskException const& e) {
    // disk write error
    job->Fail(ErrorCode::DISK_ERR, e.what());
  } catch (beast::system_error const& e) {
    // system error
    job->Fail(ErrorCode::NETWORK_ERR,
              std::string("Network interrupted: ") + e.what());
  } catch (std::exception const& e) {
    // others
    job->Fail(ErrorCode::SYSTEM_ERR,
              std::string("Unexpected system error: ") + e.what());
  }
}

};  // end anonymous namespace

FetchResult FetchFileInfo(const Url& url) {
  net::io_context ioc;
  tcp::resolver resolver(ioc);
  std::string port =
      url.port.empty() ? (url.scheme == "https" ? "443" : "80") : url.port;

  try {
    auto const dns_result = resolver.resolve(url.host, port);
    if (url.scheme == "http") {
      beast::tcp_stream stream(ioc);
      stream.connect(dns_result);
      return FetchFileInfoImpl(stream, url);
    } else if (url.scheme == "https") {
      net::ssl::context ctx(net::ssl::context::tlsv12_client);
      ctx.set_default_verify_paths();
      ctx.set_verify_mode(net::ssl::verify_peer);

      beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
      beast::get_lowest_layer(stream).connect(dns_result);

      if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
        // TODO: valid job->fail later
      }

      stream.handshake(net::ssl::stream_base::client);
      return FetchFileInfoImpl(stream, url);
    } else {
      FetchResult err;
      err.state = FetchResult::State::FAILED;
      err.data = FetchError{
          ErrorCode::NotSupported, 0,
          std::string("scheme " + url.scheme + " is not supported!")};
      return err;
    }

  } catch (beast::system_error const& e) {
    FetchResult result;
    result.state = FetchResult::State::FAILED;
    result.data = FetchError(ErrorCode::NETWORK_ERR, 0, e.what());
    return result;
  } catch (std::exception const& e) {
    FetchResult result;
    result.state = FetchResult::State::FAILED;
    result.data = FetchError(ErrorCode::SYSTEM_ERR, 0, e.what());
    return result;
  }
}

void DownloadWorker(const Task& task) {
  const auto& job = task.job;
  const auto& url = job->GetUrl();

  ConnectionGuard guard(job);

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    std::string port =
        url.port.empty() ? (url.scheme == "https" ? "443" : "80") : url.port;
    auto const results = resolver.resolve(url.host, port);

    if (url.scheme == "http") {
      DownloadWorkerImpl<beast::tcp_stream>(task, results);
    } else if (url.scheme == "https") {
      net::ssl::context ctx(net::ssl::context::tlsv12_client);
      ctx.set_default_verify_paths();
      ctx.set_verify_mode(net::ssl::verify_peer);

      DownloadWorkerImpl<beast::ssl_stream<beast::tcp_stream>>(task, results,
                                                               &ctx);
    } else {
      // scheme not supported
      job->Fail(ErrorCode::NotSupported,
                "Scheme " + url.scheme + " is not supported");
    }
  } catch (beast::system_error const& e) {
    // system error
    job->Fail(ErrorCode::NETWORK_ERR,
              std::string("DNS Resolution/Setup failed: ") + e.what());
  } catch (std::exception const& e) {
    // network error
    job->Fail(ErrorCode::SYSTEM_ERR,
              std::string("Unexpected setup error: ") + e.what());
  }
}

}  // namespace NetDownloader
}  // namespace muld