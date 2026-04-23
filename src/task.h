#pragma once

#include <muld/muld.h>
#include "download_engine.h"

namespace muld {

struct Task {
  DownloadEngine* job;
  LogCallback logger = nullptr;
};

}  // namespace muld