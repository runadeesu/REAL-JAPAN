// NOTE: this translation unit must not include raylib.h (windows.h clashes).
#include "platform/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rjc {

fs::path exeDir() {
#ifdef _WIN32
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  return fs::path(buf).parent_path();
#else
  std::error_code ec;
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::current_path() : p.parent_path();
#endif
}

fs::path dataDir() { return exeDir() / "data"; }

fs::path userDir() {
  static fs::path cached;
  if (!cached.empty()) return cached;
  const fs::path exe = exeDir();
  std::error_code ec;
  if (fs::exists(exe / "portable.txt", ec)) {
    cached = exe / "userdata";
  } else {
#ifdef _WIN32
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    cached = appdata ? fs::path(appdata) / L"RealJapan" : exe / "userdata";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) cached = fs::path(xdg) / "RealJapan";
    else if (home) cached = fs::path(home) / ".local" / "share" / "RealJapan";
    else cached = exe / "userdata";
#endif
  }
  fs::create_directories(cached / "saves", ec);
  return cached;
}

std::optional<std::vector<unsigned char>> readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) return std::nullopt;
  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> data(static_cast<size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) return std::nullopt;
  return data;
}

std::optional<std::string> readText(const fs::path& p) {
  auto d = readFile(p);
  if (!d) return std::nullopt;
  std::string s(d->begin(), d->end());
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF)
    s.erase(0, 3);  // UTF-8 BOM
  return s;
}

bool writeFileAtomic(const fs::path& p, const std::string& contents) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  return !ec;
}

std::string pathToUtf8(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string localTimestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace rjc
