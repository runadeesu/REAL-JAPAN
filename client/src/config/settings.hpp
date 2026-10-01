#pragma once
// User settings, persisted to <userDir>/settings.ini. Defaults come from
// data/config/default.ini (shipped with the game), then built-in values.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rjc {

struct Settings {
  std::string language = "ja";
  std::string world = "country";  // "country" (秋津国, fictional) or "shibuya" (real PLATEAU slice)
  int width = 1600;
  int height = 900;
  bool fullscreen = false;
  bool vsync = true;
  float fov = 70.0f;
  float mouse_sensitivity = 1.0f;
  bool invert_y = false;
  bool shadows = true;
  int view_distance_m = 1500;
  int time_scale = 30;  // game seconds per real second
  bool real_time_start = true;
  bool show_fps = false;
  bool photo_textures = true;  // PLATEAU photo textures (contain real signage / ads)
  bool post_fx = true;         // SSAO + bloom + auto exposure + FXAA
  bool head_bob = true;        // natural head movement while walking (can be turned off)
  bool motion_blur = false;    // camera motion blur (post effect; off by default)
  bool dev_overlay = false;    // developer HUD (coordinates, mesh, building data, perf); F3 toggles
  int volume = 80;             // master sound volume 0..100
  int render_height = 0;       // Android: lines drawn, scaled up to the panel (0 = the panel's own)

  static const std::vector<std::pair<int, int>>& resolutions();
  static const std::vector<int>& timeScales();
  static const std::vector<int>& viewDistances();

  void load(const std::filesystem::path& defaults, const std::filesystem::path& user);
  bool save(const std::filesystem::path& user) const;
};

}  // namespace rjc
