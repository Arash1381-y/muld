#include "url.h"

#include <algorithm>
#include <stdexcept>

namespace muld {

Url ParseUrl(const std::string& url_string) {
  Url result;
  if (url_string.empty()) {
    throw std::invalid_argument("URL cannot be empty");
  }

  std::size_t pos = 0;
  std::size_t scheme_end = url_string.find("://");
  if (scheme_end != std::string::npos) {
    result.scheme = url_string.substr(0, scheme_end);
    pos = scheme_end + 3;  // skip "://"
  } else {
    result.scheme = std::string("http");
  }

  std::size_t path_start = url_string.find('/', pos);
  std::size_t query_start = url_string.find('?', pos);
  std::size_t fragment_start = url_string.find('#', pos);
  std::size_t host_end = std::string::npos;
  if (path_start != std::string::npos) host_end = path_start;
  if (query_start != std::string::npos) {
    host_end = (host_end == std::string::npos) ? query_start
                                               : std::min(host_end, query_start);
  }
  if (fragment_start != std::string::npos) {
    host_end = (host_end == std::string::npos)
                   ? fragment_start
                   : std::min(host_end, fragment_start);
  }

  std::string host_port;
  if (host_end != std::string::npos) {
    if (host_end > pos) {
      host_port = url_string.substr(pos, host_end - pos);
      if (url_string[host_end] == '/') {
        result.path = url_string.substr(host_end);
      } else {
        result.path = url_string.substr(host_end);
        result.path.insert(result.path.begin(), '/');
      }
    } else {
      host_port = url_string.substr(pos);
      result.path = std::string("/");
    }
  } else {
    host_port = url_string.substr(pos);
    result.path = std::string("/");
  }

  std::size_t port_start = host_port.find(':');
  if (port_start != std::string::npos) {
    result.host = host_port.substr(0, port_start);
    result.port = host_port.substr(port_start + 1);
  } else {
    result.host = host_port;
    result.port = (result.scheme == "https") ? "443" : "80";
  }

  return result;
}

std::string GetUrlString(const Url& url) {
  std::string result;

  // Add scheme (must exist)
  if (!url.scheme.empty()) {
    result += url.scheme + "://";
  }

  // Add host (required)
  result += url.host;

  // Add port if custom or non-default
  if (!url.port.empty()) {
    // Omit default port for http/https
    if (!((url.scheme == "http" && url.port == "80") ||
          (url.scheme == "https" && url.port == "443"))) {
      result += ":" + url.port;
    }
  }

  // Add path
  if (!url.path.empty()) {
    // Ensure path starts with "/"
    if (url.path.front() != '/') result += "/";
    result += url.path;
  }

  return result;
}

}  // namespace muld
