// PROJECT: REAL JAPAN — native client entry point (Windows x64 / Linux).

#include <cstdlib>
#include <cstring>
#include <string>

#include "app.hpp"
#include "config/settings.hpp"
#include "platform/paths.hpp"
#include "raylib.h"

namespace {

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

  unsigned int flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT;
  if (s.vsync) flags |= FLAG_VSYNC_HINT;
  SetConfigFlags(flags);
  SetTraceLogLevel(opt.screenshot.empty() ? LOG_WARNING : LOG_INFO);
  InitWindow(s.width, s.height, "PROJECT: REAL JAPAN");
  if (!IsWindowReady()) return 2;
  if (s.fullscreen) ToggleBorderlessWindowed();
  SetExitKey(KEY_NULL);
  SetWindowMinSize(800, 600);

  int rc;
  {
    rjc::App app(opt);
    rc = app.run();
  }
  CloseWindow();
  return rc;
}
