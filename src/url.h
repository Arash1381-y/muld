#pragma once
#include <muld/muld.h>

namespace muld {

Url ParseUrl(const std::string& url_string);

std::string GetUrlString(const Url& url);

}  // namespace muld
