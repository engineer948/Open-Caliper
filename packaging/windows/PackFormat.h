// Layout of OpenCaliper-windows-x64.exe: the launcher (launcher.cpp), then the
// payload written by pack.cpp, then a fixed-size trailer.
//
// Payload, one record per file:
//   u32 pathBytes, u8 method, u64 rawSize, u64 storedSize,
//   path (UTF-8, '/' separators, relative), storedSize bytes of data
// method 0 = stored as is, 1 = LZMS (Windows Compression API, buffer mode).
// All integers little-endian, records packed with no padding.
#pragma once

#include <cstdint>
#include <cstring>

namespace ocpack {

constexpr char kMagic[8] = {'O', 'C', 'A', 'L', 'P', 'A', 'K', '1'};

enum Method : uint8_t { Stored = 0, Lzms = 1 };

#pragma pack(push, 1)
struct RecordHeader {
  uint32_t pathBytes;
  uint8_t method;
  uint64_t rawSize;
  uint64_t storedSize;
};

struct Trailer {
  uint64_t payloadSize;
  uint64_t fileCount;
  uint64_t hash; // fnv1a over the payload: detects damaged downloads, names the unpack folder
  char magic[8];
};
#pragma pack(pop)

constexpr uint64_t kFnvBasis = 14695981039346656037ull;

inline uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

} // namespace ocpack
