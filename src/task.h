#pragma once

#include "logger.h"
#include "download_job.h"

namespace muld {

struct Task {
  DownloadJob* job;
  LogCallback logger = nullptr;
};

}  // namespace muld