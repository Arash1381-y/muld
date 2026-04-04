#pragma once

#include <functional>
#include <string>

namespace muld {

enum class LogLevel { Debug, Info, Warning, Error };
using LogCallback = std::function<void(LogLevel, const std::string&)>;

}  // namespace muld