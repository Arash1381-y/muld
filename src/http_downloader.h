#pragma once
#include "file_info.h"
#include "url.h"
#include "task.h"

namespace muld {
namespace HttpDownloader {

// Fetches headers using plain HTTP
FileInfo FetchFileInfo(const Url& url);

void DownloadWorker(const Task& task);

}  // namespace HttpDownloader
}  // namespace muld
