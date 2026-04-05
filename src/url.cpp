#include "url.h"

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
  std::string host_port;
  if (path_start != std::string::npos) {
    if (path_start != std::string::npos && path_start > pos) {
      host_port = url_string.substr(pos, path_start - pos);
      result.path = url_string.substr(path_start);
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
