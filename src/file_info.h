#pragma once
#include <cstddef>

namespace muld {

struct FileInfo {
    std::size_t total_size = 0;
    bool supports_range = false;
    bool is_valid = false;
};

} // namespace muld
