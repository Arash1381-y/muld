#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace muld {

struct ChunkState {
  size_t start_range;
  size_t end_range;
  size_t downloaded;
};

struct JobImage {
  static constexpr std::uint32_t kVersion = 2;

  std::string file_path;
  size_t file_size;
  int max_connections;
  bool ranged;
  std::string url;
  std::string etag;
  std::string last_modified;
  std::uint64_t created_at = 0;
  std::uint64_t updated_at = 0;
  std::vector<ChunkState> chunks;
};

struct JobImageIndex {
  std::streamoff file_path_offset = 0;
  std::streamoff url_offset = 0;
  std::streamoff etag_offset = 0;
  std::streamoff last_modified_offset = 0;
  std::streamoff updated_at_offset = 0;
  std::streamoff chunks_offset = 0;
  size_t chunk_count = 0;
};

inline void WriteStringField(std::ofstream& out, const std::string& value,
                             std::streamoff* offset = nullptr) {
  if (offset) *offset = out.tellp();
  size_t size = value.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));
  out.write(value.data(), size);
}

inline bool WriteImageToDisk(const JobImage& img, const std::string& path,
                             JobImageIndex* index = nullptr) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  const std::uint32_t version = JobImage::kVersion;
  out.write(reinterpret_cast<const char*>(&version), sizeof(version));
  out.write(reinterpret_cast<const char*>(&img.file_size), sizeof(img.file_size));
  out.write(reinterpret_cast<const char*>(&img.max_connections),
            sizeof(img.max_connections));
  out.write(reinterpret_cast<const char*>(&img.ranged), sizeof(img.ranged));
  out.write(reinterpret_cast<const char*>(&img.created_at), sizeof(img.created_at));
  out.write(reinterpret_cast<const char*>(&img.updated_at), sizeof(img.updated_at));

  WriteStringField(out, img.file_path, index ? &index->file_path_offset : nullptr);
  WriteStringField(out, img.url, index ? &index->url_offset : nullptr);
  WriteStringField(out, img.etag, index ? &index->etag_offset : nullptr);
  WriteStringField(out, img.last_modified,
                   index ? &index->last_modified_offset : nullptr);

  if (index) {
    index->chunks_offset = out.tellp();
    index->chunk_count = img.chunks.size();
  }

  size_t size = img.chunks.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));

  if (!img.chunks.empty()) {
    out.write(reinterpret_cast<const char*>(img.chunks.data()),
              img.chunks.size() * sizeof(ChunkState));
  }

  return out.good();
}

inline bool ReadStringField(std::ifstream& in, std::string& value) {
  size_t size = 0;
  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  if (!in.good()) return false;
  value.resize(size);
  if (size > 0) in.read(value.data(), size);
  return in.good();
}

inline bool ReadImageFromDisk(JobImage& img, const std::string& path,
                              JobImageIndex* index = nullptr) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  std::uint32_t version = 0;
  in.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (!in.good()) return false;

  if (version == JobImage::kVersion) {
    in.read(reinterpret_cast<char*>(&img.file_size), sizeof(img.file_size));
    in.read(reinterpret_cast<char*>(&img.max_connections),
            sizeof(img.max_connections));
    in.read(reinterpret_cast<char*>(&img.ranged), sizeof(img.ranged));
    in.read(reinterpret_cast<char*>(&img.created_at), sizeof(img.created_at));
    in.read(reinterpret_cast<char*>(&img.updated_at), sizeof(img.updated_at));
    if (!in.good()) return false;

    if (index) index->file_path_offset = in.tellg();
    if (!ReadStringField(in, img.file_path)) return false;
    if (index) index->url_offset = in.tellg();
    if (!ReadStringField(in, img.url)) return false;
    if (index) index->etag_offset = in.tellg();
    if (!ReadStringField(in, img.etag)) return false;
    if (index) index->last_modified_offset = in.tellg();
    if (!ReadStringField(in, img.last_modified)) return false;

  } else {
    in.seekg(0);
    img = {};
    if (!ReadStringField(in, img.file_path)) return false;
    if (!ReadStringField(in, img.url)) return false;
    size_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    img.chunks.resize(size);
    if (size > 0) {
      in.read(reinterpret_cast<char*>(img.chunks.data()),
              size * sizeof(ChunkState));
    }
    return in.good();
  }

  if (index) index->chunks_offset = in.tellg();
  size_t size = 0;
  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  img.chunks.resize(size);
  if (index) index->chunk_count = size;

  if (size > 0) {
    in.read(reinterpret_cast<char*>(img.chunks.data()),
            size * sizeof(ChunkState));
  }

  return in.good();
}

inline bool UpdateImageChunksOnDisk(const std::string& path,
                                    const JobImageIndex& index,
                                    const std::vector<ChunkState>& chunks,
                                    std::uint64_t updated_at) {
  if (index.chunk_count != chunks.size()) return false;

  std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!io) return false;

  io.seekp(index.updated_at_offset);
  io.write(reinterpret_cast<const char*>(&updated_at), sizeof(updated_at));
  io.seekp(index.chunks_offset);
  const size_t size = chunks.size();
  io.write(reinterpret_cast<const char*>(&size), sizeof(size));
  if (!chunks.empty()) {
    io.write(reinterpret_cast<const char*>(chunks.data()),
             chunks.size() * sizeof(ChunkState));
  }

  return io.good();
}

}  // namespace muld
