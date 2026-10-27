// Build tool for the one-file Windows download (run by CI, not shipped):
//   opencaliper_pack <folder> <launcher.exe> <out.exe>
// Writes launcher.exe followed by every file under <folder>, compressed one by
// one, and a trailer (see PackFormat.h).
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

    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error(std::string("cannot write ") + argv[3]);
    const std::vector<char> launcher = readAll(argv[2]);
    out.write(launcher.data(), std::streamsize(launcher.size()));

    ocpack::Trailer t{};
    t.hash = ocpack::kFnvBasis;
    uint64_t rawTotal = 0;
    auto put = [&](const void* data, size_t n) {
      out.write(static_cast<const char*>(data), std::streamsize(n));
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
    out.write(reinterpret_cast<const char*>(&t), sizeof t);
    out.close();
    if (!out) throw std::runtime_error("write failed");
    std::cout << t.fileCount << " files, " << rawTotal / 1048576 << " MB -> "
              << t.payloadSize / 1048576 << " MB\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "opencaliper_pack: " << e.what() << "\n";
    return 1;
  }
}
