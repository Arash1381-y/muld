#pragma once
#include <stdexcept>
#include <string>

namespace muld {

struct Url {
  std::string scheme;  // "http" or "https"
  std::string host;    // e.g., "example.com"
  std::string port;    // "80", "443", or custom like "8080"
  std::string path;    // e.g., "/file.zip?v=1"
};

Url ParseUrl(const std::string& url_string);

}  // namespace muld
