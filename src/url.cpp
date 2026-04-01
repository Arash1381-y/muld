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
    result.scheme = "http";
  }

  std::size_t path_start = url_string.find('/', pos);
  std::string host_port;
  if (path_start != std::string::npos) {
    host_port = url_string.substr(pos, path_start - pos);
    result.path = url_string.substr(path_start);
  } else {
    host_port = url_string.substr(pos);
    result.path = "/";
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

}  // namespace muld
