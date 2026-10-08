#include "platform/logfile.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "raylib.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace rjc {

namespace {
std::mutex g_mx;
std::ofstream g_file;
std::filesystem::path g_path;
int g_console = LOG_INFO;
size_t g_lines = 0, g_bytes = 0;
const auto g_start = std::chrono::steady_clock::now();

void callback(int level, const char* fmt, va_list args) {
  char msg[2048];
  std::vsnprintf(msg, sizeof msg, fmt, args);
  const char* tag = level >= LOG_FATAL ? "FATAL" : level >= LOG_ERROR ? "ERROR" : level >= LOG_WARNING ? "WARNING" : level >= LOG_INFO ? "INFO" : "DEBUG";
  std::lock_guard<std::mutex> lk(g_mx);
  if (level >= g_console) {
#if defined(__ANDROID__)
    __android_log_write(level >= LOG_ERROR ? ANDROID_LOG_ERROR : level >= LOG_WARNING ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "raylib", msg);
#else
    std::printf("%s: %s\n", tag, msg);
#endif
  }
  if (!g_file.is_open() || g_bytes > (8u << 20)) return;  // (at most 8 MB a run)
  const bool own = std::strncmp(msg, "RJ:", 3) == 0;
  if (level < LOG_WARNING && !own && g_lines > 300) return;  // raylib's routine messages: start-up only
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  char line[2200];
  const int n = std::snprintf(line, sizeof line, "[%8.2f] %s: %s\n", t, tag, msg);
  if (n > 0) {
    g_file.write(line, std::min<int>(n, static_cast<int>(sizeof line) - 1));
    g_file.flush();  // (a run that freezes or crashes still leaves its log)
    g_bytes += static_cast<size_t>(n);
    ++g_lines;
  }
}
}  // namespace

void installLogFile(const std::filesystem::path& path, int console_level) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_console = console_level;
  g_path = path;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::path prev = path;
    prev.replace_filename("log.prev.txt");
    std::filesystem::remove(prev, ec);
    std::filesystem::rename(path, prev, ec);
  }
  g_file.open(path, std::ios::binary | std::ios::trunc);
  SetTraceLogCallback(callback);
}

const std::filesystem::path& logFilePath() { return g_path; }

namespace {
std::atomic<long long> g_beat{0};
std::atomic<const char*> g_phase{"start"};
std::thread g_watch;
std::mutex g_watch_mx;
std::condition_variable g_watch_cv;
bool g_watch_stop = false;
}  // namespace

void heartbeat() { g_beat.fetch_add(1, std::memory_order_relaxed); }
void phase(const char* what) { g_phase.store(what, std::memory_order_relaxed); }

void startWatchdog() {
  if (g_watch.joinable()) return;
  g_watch_stop = false;
  g_watch = std::thread([] {
    long long last = -1;
    auto since = std::chrono::steady_clock::now();
    double reported = 0;
    std::unique_lock<std::mutex> lk(g_watch_mx);
    while (!g_watch_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_watch_stop; })) {
      const long long b = g_beat.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      if (b != last) {
        last = b;
        since = now;
        reported = 0;
        continue;
      }
      const double stuck = std::chrono::duration<double>(now - since).count();
      if (stuck > 8.0 && stuck - reported > (reported > 0 ? 30.0 : 0.0)) {
        reported = stuck;
        TraceLog(LOG_WARNING, "RJ: main loop has not advanced for %.0f s (frame %lld, at: %s)", stuck, b, g_phase.load());
      }
    }
  });
}

void stopWatchdog() {
  {
    std::lock_guard<std::mutex> lk(g_watch_mx);
    g_watch_stop = true;
  }
  g_watch_cv.notify_all();
  if (g_watch.joinable()) g_watch.join();
}

}  // namespace rjc
