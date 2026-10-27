// Build tool for the one-file Windows download (run by CI, not shipped):
//   opencaliper_pack <folder> <launcher.exe> <out.exe>
// Copies launcher.exe to out.exe and stores every file under <folder>,
// compressed one by one, in it as a resource (see PackFormat.h).
#include "PackFormat.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<char> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: opencaliper_pack <folder> <launcher.exe> <out.exe>\n";
    return 2;
  }
  try {
    const fs::path root = argv[1];
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
      if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end()); // same input, same output

    COMPRESSOR_HANDLE comp = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &comp))
      throw std::runtime_error("CreateCompressor failed");

    std::vector<char> payload;
    ocpack::Trailer t{};
    t.hash = ocpack::kFnvBasis;
    uint64_t rawTotal = 0;
    auto put = [&](const void* data, size_t n) {
      const auto* p = static_cast<const char*>(data);
      payload.insert(payload.end(), p, p + n);
      t.hash = ocpack::fnv1a(t.hash, data, n);
      t.payloadSize += n;
    };

    for (const fs::path& f : files) {
      const std::u8string rel = fs::relative(f, root).generic_u8string();
      const std::vector<char> raw = readAll(f);
      std::vector<char> packed;
      if (!raw.empty()) {
        SIZE_T need = 0;
        Compress(comp, raw.data(), raw.size(), nullptr, 0, &need); // asks for the buffer size
        packed.resize(need);
        SIZE_T got = 0;
        // On failure the file is stored uncompressed.
        packed.resize(Compress(comp, raw.data(), raw.size(), packed.data(), packed.size(), &got) ? got : 0);
      }
      const bool store = packed.empty() || packed.size() >= raw.size();
      const std::vector<char>& data = store ? raw : packed;

      ocpack::RecordHeader h{uint32_t(rel.size()), store ? ocpack::Stored : ocpack::Lzms,
                             uint64_t(raw.size()), uint64_t(data.size())};
      put(&h, sizeof h);
      put(rel.data(), rel.size());
      put(data.data(), data.size());
      ++t.fileCount;
      rawTotal += raw.size();
    }
    CloseCompressor(comp);

    std::memcpy(t.magic, ocpack::kMagic, sizeof t.magic);
    const auto* tp = reinterpret_cast<const char*>(&t);
    payload.insert(payload.end(), tp, tp + sizeof t);

    const fs::path out = argv[3];
    fs::copy_file(argv[2], out, fs::copy_options::overwrite_existing);
    HANDLE res = BeginUpdateResourceW(out.c_str(), FALSE);
    if (!res) throw std::runtime_error("BeginUpdateResource failed");
    if (!UpdateResourceW(res, MAKEINTRESOURCEW(ocpack::kRcData), ocpack::kResourceName,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), payload.data(), DWORD(payload.size()))) {
      EndUpdateResourceW(res, TRUE);
      throw std::runtime_error("UpdateResource failed");
    }
    if (!EndUpdateResourceW(res, FALSE)) throw std::runtime_error("EndUpdateResource failed");
    std::cout << t.fileCount << " files, " << rawTotal / 1048576 << " MB -> "
              << t.payloadSize / 1048576 << " MB\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "opencaliper_pack: " << e.what() << "\n";
    return 1;
  }
}
