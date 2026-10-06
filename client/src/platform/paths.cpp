// NOTE: this translation unit must not include raylib.h (windows.h clashes).
#include "platform/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
extern "C" struct android_app* GetAndroidApp(void);  // (raylib, rcore_android.c)
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

#if defined(__ANDROID__)
// Android: the game data stays inside the APK (stored uncompressed) and is read through the asset
// manager; dataDir() is the marker "apk:/data" that readFile() and friends recognise.
namespace {
constexpr const char* kApk = "apk:/";
AAssetManager* assets() {
  struct android_app* app = GetAndroidApp();
  return app && app->activity ? app->activity->assetManager : nullptr;
}
bool isApk(const fs::path& p, std::string& name) {
  const std::string s = p.generic_string();
  if (s.rfind(kApk, 0) != 0) return false;
  name = s.substr(5);
  return true;
}
std::optional<std::vector<unsigned char>> readAsset(const std::string& name) {
  AAssetManager* m = assets();
  AAsset* a = m ? AAssetManager_open(m, name.c_str(), AASSET_MODE_BUFFER) : nullptr;
  if (!a) return std::nullopt;
  const off64_t n = AAsset_getLength64(a);
  std::vector<unsigned char> data(static_cast<size_t>(std::max<off64_t>(n, 0)));
  const void* buf = AAsset_getBuffer(a);
  bool ok = true;
  if (buf) std::memcpy(data.data(), buf, data.size());
  else ok = AAsset_read(a, data.data(), data.size()) == static_cast<int>(data.size());
  AAsset_close(a);
  if (!ok) return std::nullopt;
  return data;
}
}  // namespace

fs::path dataDir() { return fs::path("apk:") / "data"; }

fs::path userDir() {
  static fs::path cached;
  if (!cached.empty()) return cached;
  struct android_app* app = GetAndroidApp();
  cached = app && app->activity && app->activity->internalDataPath ? fs::path(app->activity->internalDataPath) : fs::path("/sdcard/RealJapan");
  std::error_code ec;
  fs::create_directories(cached / "saves", ec);
  return cached;
}
#else
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
#endif

bool fileExists(const fs::path& p) {
#if defined(__ANDROID__)
  if (std::string name; isApk(p, name)) {
    AAssetManager* m = assets();
    AAsset* a = m ? AAssetManager_open(m, name.c_str(), AASSET_MODE_STREAMING) : nullptr;
    if (a) AAsset_close(a);
    return a != nullptr;
  }
#endif
  std::error_code ec;
  return fs::exists(p, ec);
}

std::vector<fs::path> listFiles(const fs::path& dir) {
  std::vector<fs::path> out;
#if defined(__ANDROID__)
  if (std::string name; isApk(dir, name)) {
    AAssetManager* m = assets();
    AAssetDir* d = m ? AAssetManager_openDir(m, name.c_str()) : nullptr;
    if (!d) return out;
    while (const char* f = AAssetDir_getNextFileName(d)) out.push_back(dir / f);
    AAssetDir_close(d);
    return out;
  }
#endif
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) out.push_back(e.path());
  return out;
}

std::optional<std::vector<unsigned char>> readFile(const fs::path& p) {
#if defined(__ANDROID__)
  if (std::string name; isApk(p, name)) return readAsset(name);
#endif
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

std::vector<fs::path> userCharacterDirs() {
  std::vector<fs::path> out;
#if defined(__ANDROID__)
  struct android_app* app = GetAndroidApp();
  if (app && app->activity && app->activity->externalDataPath) out.push_back(fs::path(app->activity->externalDataPath) / "characters");
  out.push_back(userDir() / "characters");
#else
  out.push_back(exeDir() / "characters");
  out.push_back(userDir() / "characters");
#endif
  return out;
}

bool openFolder(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
#if defined(_WIN32)
  return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__ANDROID__)
  (void)dir;
  return false;
#else
  std::string s = dir.string();
  if (s.find('\'') != std::string::npos) return false;
  const std::string cmd = "xdg-open '" + s + "' >/dev/null 2>&1 &";
  return std::system(cmd.c_str()) == 0;
#endif
}

}  // namespace rjc
