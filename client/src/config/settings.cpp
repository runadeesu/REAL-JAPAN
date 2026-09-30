#include "config/settings.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include "platform/paths.hpp"
#include "util/text.hpp"

namespace rjc {
namespace {

void applyKv(Settings& s, const std::map<std::string, std::string>& kv) {
  auto get = [&](const char* k) -> const std::string* {
    auto it = kv.find(k);
    return it == kv.end() ? nullptr : &it->second;
  };
  auto b = [&](const char* k, bool& v) {
    if (auto x = get(k)) v = (*x == "1" || *x == "true" || *x == "on");
  };
  auto i = [&](const char* k, int& v) {
    if (auto x = get(k)) try { v = std::stoi(*x); } catch (...) {}
  };
  auto f = [&](const char* k, float& v) {
    if (auto x = get(k)) try { v = std::stof(*x); } catch (...) {}
  };
  if (auto x = get("language")) s.language = (*x == "en") ? "en" : "ja";
  if (auto x = get("world")) s.world = (*x == "shibuya") ? "shibuya" : "country";  // ("island": the earlier fictional world)
  i("width", s.width);
  i("height", s.height);
  b("fullscreen", s.fullscreen);
  b("vsync", s.vsync);
  f("fov", s.fov);
  f("mouse_sensitivity", s.mouse_sensitivity);
  b("invert_y", s.invert_y);
  b("shadows", s.shadows);
  i("view_distance_m", s.view_distance_m);
  i("time_scale", s.time_scale);
  b("real_time_start", s.real_time_start);
  b("show_fps", s.show_fps);
  b("photo_textures", s.photo_textures);
  b("post_fx", s.post_fx);
  b("head_bob", s.head_bob);
  b("dev_overlay", s.dev_overlay);
  i("volume", s.volume);
  s.volume = std::clamp(s.volume, 0, 100);
  i("render_height", s.render_height);
  s.render_height = std::clamp(s.render_height, 0, 4320);
  s.width = std::clamp(s.width, 800, 7680);
  s.height = std::clamp(s.height, 600, 4320);
  s.fov = std::clamp(s.fov, 50.0f, 100.0f);
  s.mouse_sensitivity = std::clamp(s.mouse_sensitivity, 0.1f, 5.0f);
  s.view_distance_m = std::clamp(s.view_distance_m, 500, 4000);
  s.time_scale = std::clamp(s.time_scale, 1, 600);
}

}  // namespace

const std::vector<std::pair<int, int>>& Settings::resolutions() {
  static const std::vector<std::pair<int, int>> r{{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
  return r;
}
const std::vector<int>& Settings::timeScales() {
  static const std::vector<int> t{1, 10, 30, 60, 120};
  return t;
}
const std::vector<int>& Settings::viewDistances() {
  static const std::vector<int> v{800, 1500, 2500, 4000};
  return v;
}

void Settings::load(const std::filesystem::path& defaults, const std::filesystem::path& user) {
  if (auto t = readText(defaults)) applyKv(*this, parseKeyValue(*t));
#if defined(__ANDROID__)
  if (auto t = readText(defaults.parent_path() / "android.ini")) applyKv(*this, parseKeyValue(*t));  // lighter phone defaults
#endif
  if (auto t = readText(user)) applyKv(*this, parseKeyValue(*t));
}

bool Settings::save(const std::filesystem::path& user) const {
  std::ostringstream o;
  o << "# PROJECT: REAL JAPAN user settings\n"
    << "language = " << language << "\n"
    << "width = " << width << "\nheight = " << height << "\n"
    << "fullscreen = " << (fullscreen ? 1 : 0) << "\n"
    << "vsync = " << (vsync ? 1 : 0) << "\n"
    << "fov = " << fov << "\n"
    << "mouse_sensitivity = " << mouse_sensitivity << "\n"
    << "invert_y = " << (invert_y ? 1 : 0) << "\n"
    << "shadows = " << (shadows ? 1 : 0) << "\n"
    << "view_distance_m = " << view_distance_m << "\n"
    << "time_scale = " << time_scale << "\n"
    << "real_time_start = " << (real_time_start ? 1 : 0) << "\n"
    << "show_fps = " << (show_fps ? 1 : 0) << "\n"
    << "photo_textures = " << (photo_textures ? 1 : 0) << "\n"
    << "post_fx = " << (post_fx ? 1 : 0) << "\n"
    << "head_bob = " << (head_bob ? 1 : 0) << "\n"
    << "dev_overlay = " << (dev_overlay ? 1 : 0) << "\n"
    << "volume = " << volume << "\n"
    << "render_height = " << render_height << "\n"
    << "world = " << world << "\n";
  return writeFileAtomic(user, o.str());
}

}  // namespace rjc
