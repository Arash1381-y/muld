#pragma once

#include "file_info.h"
#include "task.h"
#include "url.h"

namespace muld {
namespace NetDownloader {

// Fetches headers using plain HTTP
FetchResult FetchFileInfo(const Url& url);

void DownloadWorker(const Task& task);

}  // namespace NetDownloader
}  // namespace muld
