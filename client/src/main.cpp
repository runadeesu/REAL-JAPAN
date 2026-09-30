// PROJECT: REAL JAPAN — native client entry point (Windows x64 / Linux / Android).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.hpp"
#include "config/settings.hpp"
#include "platform/paths.hpp"
#include "raylib.h"

#if defined(__ANDROID__)
#include <android_native_app_glue.h>
extern "C" struct android_app* GetAndroidApp(void);  // (raylib, rcore_android.c)
#endif

namespace {

#if defined(__ANDROID__)
// raylib handles the activity's lifecycle; on top of that, pausing (home button, a call, the screen
// going off) writes the autosave. The callback runs on the game's own thread, inside raylib's
// event polling, so the save is safe here.
rjc::App* g_app = nullptr;
void (*g_raylib_cmd)(struct android_app*, int32_t) = nullptr;
void onAppCmd(struct android_app* a, int32_t cmd) {
  if (g_raylib_cmd) g_raylib_cmd(a, cmd);
  if (cmd == APP_CMD_PAUSE && g_app) g_app->autosave();
}
#endif

rjc::LaunchOptions parseArgs(int argc, char** argv) {
  rjc::LaunchOptions o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
    if (a == "--screenshot") o.screenshot = next();
    else if (a == "--frames") o.frames = std::atoi(next().c_str());
    else if (a == "--state") o.state = next();
    else if (a == "--time") o.time_jst = next();
    else if (a == "--lang") o.lang = next();
    else if (a == "--yaw") o.yaw_deg = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--pitch") o.pitch_deg = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--fly") o.fly = true;
    else if (a == "--alt") o.alt = std::atof(next().c_str());
    else if (a == "--third-person") o.camera_mode = 1;
    else if (a == "--timescale") o.time_scale = std::atoi(next().c_str());
    else if (a == "--selftest") o.selftest = true;
    else if (a == "--autowalk") o.autowalk = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--walk") o.walk = next();
    else if (a == "--entrance") o.entrance = std::atoi(next().c_str());
    else if (a == "--weather") o.weather = next();
    else if (a == "--dev") o.dev = true;
    else if (a == "--world") o.world = next();
    else if (a == "--drive") o.drive = next();
    else if (a == "--station") o.station = std::atoi(next().c_str());
    else if (a == "--ride") o.ride = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--alight") o.alight = true;
    else if (a == "--simspeed") o.sim_speed = std::max(1, std::atoi(next().c_str()));
    else if (a == "--fly-script") o.fly_script = next();
    else if (a == "--audio-wav") o.audio_wav = next();
    else if (a == "--pos") {
      const std::string v = next();
      if (std::sscanf(v.c_str(), "%lf,%lf", &o.lat, &o.lon) == 2) o.has_pos = true;
    }
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  rjc::LaunchOptions opt = parseArgs(argc, argv);
  rjc::Settings s;
  s.load(rjc::dataDir() / "config" / "default.ini", rjc::userDir() / "settings.ini");

#if defined(__ANDROID__)
  // Full screen, landscape. The game draws `render_height` lines (0: the panel's own) and the system
  // scales the picture up to the panel; the width follows the panel's shape (patched raylib, see
  // tools/package_android.sh).
  SetConfigFlags(FLAG_FULLSCREEN_MODE);
  SetTraceLogLevel(LOG_INFO);
  InitWindow(0, std::max(0, s.render_height), "PROJECT: REAL JAPAN");
  if (!IsWindowReady()) return 2;
  SetExitKey(KEY_NULL);
#else
  unsigned int flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT;
  if (s.vsync) flags |= FLAG_VSYNC_HINT;
  SetConfigFlags(flags);
  SetTraceLogLevel(opt.screenshot.empty() && !opt.selftest ? LOG_WARNING : (std::getenv("RJ_DEBUG") ? LOG_DEBUG : LOG_INFO));
  InitWindow(s.width, s.height, "PROJECT: REAL JAPAN");
  if (!IsWindowReady()) return 2;
  if (s.fullscreen) ToggleBorderlessWindowed();
  SetExitKey(KEY_NULL);
  SetWindowMinSize(800, 600);
#endif

  int rc;
  {
    rjc::App app(opt);
#if defined(__ANDROID__)
    if (struct android_app* a = GetAndroidApp()) {
      g_app = &app;
      g_raylib_cmd = a->onAppCmd;
      a->onAppCmd = onAppCmd;
    }
    rc = app.run();
    if (struct android_app* a = GetAndroidApp(); a && a->onAppCmd == onAppCmd) a->onAppCmd = g_raylib_cmd;
    g_app = nullptr;
#else
    rc = app.run();
#endif
  }
  CloseWindow();
  return rc;
}
