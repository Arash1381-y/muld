#pragma once
#include <fstream>
#include <string>
#include <vector>

namespace muld {

struct UnFinishedChunk {
  size_t start_range;
  size_t end_range;
};

struct JobImage {
  std::string file_path;
  std::string url;
  std::vector<UnFinishedChunk> chunks;
};

inline bool WriteImageToDisk(const JobImage& img, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;

  size_t size;

  size = img.file_path.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));
  out.write(img.file_path.data(), size);

  size = img.url.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));
  out.write(img.url.data(), size);

  size = img.chunks.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));

  if (!img.chunks.empty()) {
    out.write(reinterpret_cast<const char*>(img.chunks.data()),
              img.chunks.size() * sizeof(UnFinishedChunk));
  }

  return out.good();
}

inline bool ReadImageFromDisk(JobImage& img, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  size_t size;

  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  img.file_path.resize(size);
  in.read(img.file_path.data(), size);

  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  img.url.resize(size);
  in.read(img.url.data(), size);

  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  img.chunks.resize(size);

  if (size > 0) {
    in.read(reinterpret_cast<char*>(img.chunks.data()),
            size * sizeof(UnFinishedChunk));
  }

  return in.good();
}

}  // namespace muld
