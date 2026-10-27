// OpenCaliper-windows-x64.exe: the one-file Windows download.
// The program folder (the same as in the zip) is stored in this launcher as a
// resource (see pack.cpp). On the first start it is unpacked to
//   %LOCALAPPDATA%\open-workshop-tools\OpenCaliper\app\<version>-<hash>
// and every start runs OpenCaliper.exe from there with the same arguments.
// Nothing is installed or registered; folders of other versions are removed.
//
// OPENCALIPER_UNPACK_ONLY=1 in the environment: unpack, don't start, report
// errors only through the exit code (used by CI).
#include "PackFormat.h"

#include <windows.h>
#include <compressapi.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::runtime_error winError(const char* what) {
  return std::runtime_error(std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")");
}

fs::path localAppData() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return buf;
  return fs::temp_directory_path();
}

std::wstring folderName(uint64_t hash) {
  std::wstring s;
  for (const char* v = OPENCALIPER_VERSION; *v; ++v) s += wchar_t(*v);
  s += L'-';
  for (int shift = 60; shift >= 0; shift -= 4) s += L"0123456789abcdef"[(hash >> shift) & 15];
  return s;
}

// Removes everything in `base` except `keep`. A folder whose program is still
// running can't be renamed, so it is left alone; renaming first means a folder
// is never left half-deleted under its real name.
void removeOthers(const fs::path& base, const std::wstring& keep) {
  std::error_code ec;
  std::vector<fs::path> others;
  for (const auto& e : fs::directory_iterator(base, ec))
    if (e.path().filename() != keep) others.push_back(e.path());
  int n = 0;
  for (const fs::path& p : others) {
    if (p.filename().wstring().starts_with(L".trash")) {
      fs::remove_all(p, ec);
      continue;
    }
    const fs::path trash = base / (L".trash-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(n++));
    fs::rename(p, trash, ec);
    if (!ec) fs::remove_all(trash, ec);
  }
}

// Reads the records in order, hashing everything it hands out.
class Payload {
public:
  explicit Payload(std::span<const char> data) : data_(data) {}

  const char* take(uint64_t n) {
    if (n > data_.size()) throw std::runtime_error("the download is damaged (payload too short)");
    const char* p = data_.data();
    data_ = data_.subspan(size_t(n));
    hash_ = ocpack::fnv1a(hash_, p, size_t(n));
    return p;
  }
  uint64_t left() const { return data_.size(); }
  uint64_t hash() const { return hash_; }

private:
  std::span<const char> data_;
  uint64_t hash_ = ocpack::kFnvBasis;
};

void unpack(std::span<const char> payload, const ocpack::Trailer& t, const fs::path& tmp, const fs::path& dir) {
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  DECOMPRESSOR_HANDLE dec = nullptr;
  if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &dec)) throw winError("CreateDecompressor");
  struct Close { DECOMPRESSOR_HANDLE h; ~Close() { CloseDecompressor(h); } } closeDec{dec};

  Payload p(payload);
  std::vector<char> raw;
  for (uint64_t i = 0; i < t.fileCount; ++i) {
    ocpack::RecordHeader h;
    std::memcpy(&h, p.take(sizeof h), sizeof h);
    if (h.pathBytes == 0 || h.pathBytes > 4096 || h.storedSize > p.left())
      throw std::runtime_error("the download is damaged (bad file entry)");
    const char* namePtr = p.take(h.pathBytes);
    const fs::path rel(std::u8string(reinterpret_cast<const char8_t*>(namePtr), h.pathBytes));
    bool safe = rel.is_relative() && !rel.has_root_name() && !rel.has_root_directory();
    for (const fs::path& part : rel) safe = safe && part != L"..";
    if (!safe) throw std::runtime_error("the download is damaged (bad file name)");

    std::span<const char> data(p.take(h.storedSize), size_t(h.storedSize));
    if (h.method == ocpack::Lzms) {
      raw.resize(size_t(h.rawSize));
      SIZE_T got = 0;
      if (!Decompress(dec, data.data(), data.size(), raw.data(), raw.size(), &got) || got != raw.size())
        throw std::runtime_error("the download is damaged (cannot decompress)");
      data = raw;
    } else if (h.method != ocpack::Stored || h.storedSize != h.rawSize) {
      throw std::runtime_error("the download is damaged (unknown entry)");
    }

    const fs::path dst = tmp / rel;
    fs::create_directories(dst.parent_path());
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    out.close();
    if (!out) throw std::runtime_error("cannot write " + dst.string() + " (is the disk full?)");
  }
  if (p.left() != 0 || p.hash() != t.hash)
    throw std::runtime_error("the download is damaged; please download OpenCaliper again");
  fs::rename(tmp, dir); // the folder appears complete or not at all
}

// Runs unpack() on a worker thread while a small "Preparing" window is shown.
void unpackWithWindow(std::span<const char> payload, const ocpack::Trailer& t, const fs::path& tmp,
                      const fs::path& dir) {
  RECT work{0, 0, 1024, 768};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int w = 380, h = 80;
  HWND win = CreateWindowExW(0, L"STATIC", L"Preparing OpenCaliper (first start only)\x2026",
                             WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                             (work.left + work.right - w) / 2, (work.top + work.bottom - h) / 2, w, h,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (win) {
    SendMessageW(win, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    UpdateWindow(win);
  }

  std::exception_ptr error;
  std::thread worker([&] {
    try {
      unpack(payload, t, tmp, dir);
    } catch (...) {
      error = std::current_exception();
    }
  });
  HANDLE th = worker.native_handle();
  while (MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  worker.join();
  if (win) DestroyWindow(win);
  if (error) std::rethrow_exception(error);
}

void launch(const fs::path& exe) {
  // Pass on our own arguments: everything after the program name.
  const wchar_t* args = GetCommandLineW();
  if (*args == L'"') {
    ++args;
    while (*args && *args != L'"') ++args;
    if (*args) ++args;
  } else {
    while (*args && *args != L' ' && *args != L'\t') ++args;
  }
  std::wstring cmd = L"\"" + exe.wstring() + L"\"" + args;

  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    throw winError("Starting OpenCaliper.exe");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

void run(bool unpackOnly) {
  const char* damaged = "the download is damaged; please download OpenCaliper again";
  HRSRC res = FindResourceW(nullptr, ocpack::kResourceName, MAKEINTRESOURCEW(ocpack::kRcData));
  HGLOBAL loaded = res ? LoadResource(nullptr, res) : nullptr;
  const auto* bytes = static_cast<const char*>(loaded ? LockResource(loaded) : nullptr);
  const uint64_t size = res ? SizeofResource(nullptr, res) : 0;
  ocpack::Trailer t{};
  if (!bytes || size < sizeof t) throw std::runtime_error(damaged);
  std::memcpy(&t, bytes + size - sizeof t, sizeof t);
  if (std::memcmp(t.magic, ocpack::kMagic, sizeof t.magic) != 0 || t.payloadSize != size - sizeof t)
    throw std::runtime_error(damaged);
  const std::span<const char> payload(bytes, size_t(t.payloadSize));

  const fs::path base = localAppData() / L"open-workshop-tools" / L"OpenCaliper" / L"app";
  const std::wstring name = folderName(t.hash);
  const fs::path dir = base / name;

  // One launcher at a time unpacks or cleans up (double-clicks, several versions).
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\OpenCaliperUnpack");
  if (!mutex) throw winError("CreateMutex");
  WaitForSingleObject(mutex, INFINITE);
  struct Release { HANDLE h; ~Release() { ReleaseMutex(h); CloseHandle(h); } } release{mutex};

  fs::create_directories(base);
  removeOthers(base, name);
  if (!fs::exists(dir / L"OpenCaliper.exe")) {
    fs::remove_all(dir); // a folder someone emptied by hand
    const fs::path tmp = base / (name + L".partial");
    if (unpackOnly) unpack(payload, t, tmp, dir);
    else unpackWithWindow(payload, t, tmp, dir);
  }
  if (!unpackOnly) launch(dir / L"OpenCaliper.exe");
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const bool unpackOnly = GetEnvironmentVariableW(L"OPENCALIPER_UNPACK_ONLY", nullptr, 0) > 0;
  try {
    run(unpackOnly);
    return 0;
  } catch (const std::exception& e) {
    if (!unpackOnly) {
      const std::string msg = std::string("OpenCaliper could not start:\n\n") + e.what();
      std::wstring wmsg(size_t(MultiByteToWideChar(CP_ACP, 0, msg.c_str(), -1, nullptr, 0)), L'\0');
      MultiByteToWideChar(CP_ACP, 0, msg.c_str(), -1, wmsg.data(), int(wmsg.size()));
      MessageBoxW(nullptr, wmsg.c_str(), L"OpenCaliper", MB_OK | MB_ICONERROR);
    }
    return 1;
  }
}
