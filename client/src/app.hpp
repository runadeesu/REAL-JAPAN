#pragma once
// Application: screens, game session, save/load, settings, localisation.

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "game/pedestrians.hpp"
#include "game/player.hpp"
#include "game/town_sim.hpp"
#include "game/traffic_signals.hpp"
#include "game/weather.hpp"
#include "i18n/i18n.hpp"
#include "render/renderer.hpp"
#include "rj/econ/ledger.hpp"
#include "rj/sim/calendar.hpp"
#include "ui/ui.hpp"
#include "world/world.hpp"

namespace rjc {

struct LaunchOptions {
  std::string screenshot;  // write a PNG after `frames` frames in `state`, then quit
  int frames = 120;
  std::string state;       // title | game | pause | phone:map | phone:town | phone:clock | settings | credits
  std::string time_jst;    // "YYYY-MM-DDTHH:MM"
  bool has_pos = false;
  double lat = 0, lon = 0;
  float yaw_deg = NAN, pitch_deg = NAN;
  std::string lang;
  bool fly = false;
  double alt = 0;
  int camera_mode = -1;
  int time_scale = 0;  // override (0 = use settings)
  bool selftest = false;  // run automated checks against the real game systems, then quit
  float autowalk = 0.0f;  // test aid: walk forward for N seconds
  std::string walk;       // test aid: scripted legs "yaw_deg:seconds,yaw_deg:seconds,..."
  int entrance = -1;      // --state interior: which entrance to use (-1 = nearest to spawn)
  std::string weather;    // force a weather state (clear, fair, thin_cloud, overcast, light_rain, rain, heavy_rain, thunder, fog, windy)
  bool dev = false;       // start with the developer overlay
};

class App {
 public:
  explicit App(LaunchOptions o) : opt_(std::move(o)) {}
  int run();

 private:
  enum class Screen { Boot, Loading, Title, Settings, Slots, Credits, Game, Pause, Phone, Fatal };
  enum class PhoneApp { Home, Map, Clock, Wallet, Town };

  bool boot();
  void shutdown();
  void update(float dt);
  void draw();

  void drawWorldView(const Camera3D& cam);
  void drawBootScreen();
  void drawLoading();
  void drawTitle();
  void drawSettings();
  void drawSlots();
  void drawCredits();
  void drawHud();
  void drawBuildingInfo();
  void drawWalkerInfo();
  void drawPause();
  void drawPhone();
  void drawMap(Rectangle r, double half_extent_m, bool labels);
  void drawFatal();
  void drawToast(float dt);

  void newGame();
  bool loadSlot(int slot);
  bool saveSlot(int slot);
  void endSession();
  void setLanguage(const std::string& code);
  void applyWindowMode();
  void saveSettings();
  void toast(const std::string& msg);
  void takeUserScreenshot();
  void applyLaunchOverrides();
  void runSelfTest();
  int exit_code_ = 0;

  rj::sim::CivilDateTime jst() const { return clock_.jst(); }
  std::string dateTimeString() const;
  std::string tr(const std::string& k) const { return i18n_.tr(k); }
  Camera3D titleCamera() const;

  LaunchOptions opt_;
  Settings settings_;
  I18n i18n_;
  Ui ui_;
  World world_;
  Renderer renderer_;
  Player player_;
  rj::sim::GameClock clock_{0};
  std::unique_ptr<rj::econ::Ledger> ledger_;
  rj::econ::AccountId player_account_ = 0;
  TownSim town_;
  Pedestrians peds_;
  const Walker* hover_walker_ = nullptr;
  std::string inside_id_;  // interior the player is in (empty = outside)
  std::string prompt_;     // context action shown on the HUD (E key)
  const Interior* insideInterior() const { return inside_id_.empty() ? nullptr : world_.interior(inside_id_); }
  float underground_ = 0.0f;  // 0 = eye at/above the street surface, 1 = fully underground (lighting blend)
  rj::geo::Vec3d walk_start_;
  std::vector<std::pair<float, float>> walk_legs_;  // scripted walk (yaw_deg, seconds), test aid
  void updateInteriorAction();
  void drawInteriorInfo();
  std::filesystem::path slice_dir_;

  Screen screen_ = Screen::Boot;
  Screen settings_return_ = Screen::Title;
  Screen slots_return_ = Screen::Title;
  bool slots_saving_ = false;
  PhoneApp phone_app_ = PhoneApp::Home;
  bool in_session_ = false;
  bool cursor_locked_ = false;
  double play_seconds_ = 0;
  double autosave_timer_ = 0;
  std::string toast_;
  float toast_t_ = 0;
  float title_t_ = 0;
  float session_t_ = 0;
  std::string fatal_;
  int frame_ = 0;
  int shot_frames_ = -1;
  std::optional<World::Hit> hover_;
  bool quit_ = false;
  double map_half_extent_ = 350.0;
  Lighting lighting_;
  WeatherSim weather_;
  FacadeDetail facades_;
  TrafficSignals signals_;
  int64_t weather_prev_unix_ = 0;
  float render_time_ = 0.0f;
  std::vector<PointLight> collectLights(const Camera3D& cam) const;
};

}  // namespace rjc
