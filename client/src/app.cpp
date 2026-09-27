#include "app.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <set>

#include "platform/paths.hpp"
#include "raymath.h"
#include "rj/env/solar.hpp"
#include "rj/geo/mesh_code.hpp"
#include "save/save.hpp"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

constexpr int64_t kStartMoney = 30000;
constexpr double kAutosaveInterval = 300.0;

std::string two(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }

bool parseJst(const std::string& s, rj::sim::CivilDate& d, int& hh, int& mm) {
  return std::sscanf(s.c_str(), "%d-%d-%dT%d:%d", &d.y, &d.m, &d.d, &hh, &mm) == 5;
}

}  // namespace

// ---------------------------------------------------------------------------
int App::run() {
  while (!quit_) {
    if (WindowShouldClose()) {
      if (in_session_) saveSlot(kAutosaveSlot);
      break;
    }
    const float dt = GetFrameTime();
    update(dt);
    BeginDrawing();
    draw();
    drawToast(dt);
    EndDrawing();
    ++frame_;
    const bool walking = scriptBusy();  // scripted walk / drive / ride finishes first
    if (shot_frames_ >= 0 && !walking && --shot_frames_ < 0) {
      Image img = LoadImageFromScreen();
      ExportImage(img, opt_.screenshot.c_str());
      UnloadImage(img);
      TraceLog(LOG_INFO, "RJ: screenshot written: %s", opt_.screenshot.c_str());
      quit_ = true;
    }
  }
  shutdown();
  return !fatal_.empty() ? 1 : exit_code_;
}

bool App::boot() {
  const auto data = dataDir();
  settings_.load(data / "config" / "default.ini", userDir() / "settings.ini");
  if (!opt_.lang.empty()) settings_.language = opt_.lang;
  if (opt_.time_scale > 0) settings_.time_scale = opt_.time_scale;
  if (!i18n_.load(data / "lang", settings_.language)) {
    fatal_ = "Missing language files in " + pathToUtf8(data / "lang");
    return false;
  }
  saved_world_ = settings_.world;  // a --world launch option is not written back to settings.ini
  if (!opt_.world.empty()) settings_.world = opt_.world == "shibuya" ? "shibuya" : "island";
  slice_dir_ = data / "world" / settings_.world;
  if (!std::filesystem::exists(slice_dir_ / "client.txt")) slice_dir_ = data / "world" / "shibuya";  // island not cooked
  std::string err;
  if (!world_.loadMeta(slice_dir_, err)) {
    fatal_ = "World data error: " + err;
    return false;
  }
  town_.load(slice_dir_ / "residents.csv");
  {
    std::string terr;
    if (!traffic_.load(slice_dir_ / "roads.rjroad", terr)) TraceLog(LOG_WARNING, "RJ: traffic disabled: %s", terr.c_str());
    std::string rerr;
    if (std::filesystem::exists(slice_dir_ / "rail.txt") && !trains_.load(slice_dir_ / "rail.txt", rerr))
      TraceLog(LOG_WARNING, "RJ: trains disabled: %s", rerr.c_str());
    std::string ferr;
    if (std::filesystem::exists(slice_dir_ / "transport.txt") && !ferries_.load(slice_dir_ / "transport.txt", ferr))
      TraceLog(LOG_WARNING, "RJ: ferries disabled: %s", ferr.c_str());
    std::string aerr;
    if (std::filesystem::exists(slice_dir_ / "transport.txt") && !aviation_.load(slice_dir_ / "transport.txt", aerr))
      TraceLog(LOG_WARNING, "RJ: aviation disabled: %s", aerr.c_str());
  }
  // Glyphs: every language file + real building names + resident names + ASCII.
  std::set<int> cps;
  for (size_t i = 0; i < town_.size(); ++i) collectCodepoints(town_.npc(i).fullName(), cps);
  for (const auto& o : rj::sim::allOccupations()) collectCodepoints(std::string(o.name_ja), cps);
  for (int c = 32; c < 127; ++c) cps.insert(c);
  i18n_.collectAllCodepoints(data / "lang", cps);
  for (const auto& p : world_.meta().pois) collectCodepoints(p.name, cps);
  for (const auto& s : world_.meta().sources) collectCodepoints(s.attribution, cps);
  collectCodepoints(world_.meta().name_ja + "°×・…→←↑↓〜「」（）、。！？：年月日時分秒円¥•–—", cps);
  if (!ui_.loadFont(data / "fonts" / "BIZUDPGothic-Regular.ttf", cps, 44)) {
    fatal_ = "Font load failed: data/fonts/BIZUDPGothic-Regular.ttf";
    return false;
  }
  {
    // The distribution may come as two ZIP parts: say clearly when one was not extracted.
    std::string missing;
    std::error_code ec;
    for (const auto& c : world_.meta().cells)
      if (!std::filesystem::exists(slice_dir_ / "cells" / (c.mesh + ".rjcell"), ec)) missing += " " + c.mesh + ".rjcell";
    if (!missing.empty()) {
      fatal_ = tr("error.missing_cells") + "\n\n" + pathToUtf8(slice_dir_ / "cells") + ":" + missing;
      return false;
    }
  }
  if (!renderer_.init()) {
    fatal_ = "Shader compilation failed (OpenGL 3.3 required)";
    return false;
  }
  if (auto icon = readFile(data / "icon.png")) {
    Image img = LoadImageFromMemory(".png", icon->data(), static_cast<int>(icon->size()));
    if (img.data) {
      SetWindowIcon(img);
      UnloadImage(img);
    }
  }
  // Stream the slice around the spawn point; the title screen flies over it.
  const auto& m = world_.meta();
  double h0 = 0;
  for (const auto& c : m.cells)
    if (rj::geo::MeshCode::fromLatLon({m.spawn_lat, m.spawn_lon}, 3)->str() == c.mesh) h0 = c.h;
  world_.resetOrigin({m.spawn_lat, m.spawn_lon, h0});
  player_.pos = {0, 0, 0};
  player_.yaw = static_cast<float>(m.spawn_heading * DEG2RAD);
  clock_ = rj::sim::GameClock(static_cast<int64_t>(std::time(nullptr)), settings_.time_scale);
  return true;
}

void App::shutdown() {
  world_.unloadAll();
  renderer_.shutdown();
  ui_.unload();
}

// ---------------------------------------------------------------------------
void App::setLanguage(const std::string& code) {
  settings_.language = code;
  i18n_.load(dataDir() / "lang", code);
  saveSettings();
}

void App::saveSettings() {
  Settings s = settings_;
  if (!opt_.world.empty()) s.world = saved_world_;
  s.save(userDir() / "settings.ini");
}

void App::applyWindowMode() {
  const bool is_full = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
  if (settings_.fullscreen != is_full) ToggleBorderlessWindowed();
  if (!settings_.fullscreen) SetWindowSize(settings_.width, settings_.height);
  if (settings_.vsync) SetWindowState(FLAG_VSYNC_HINT);
  else ClearWindowState(FLAG_VSYNC_HINT);
}

void App::toast(const std::string& msg) {
  toast_ = msg;
  toast_t_ = 3.0f;
}

std::string App::dateTimeString() const {
  const auto t = jst();
  const int wd = rj::sim::weekday(t.date);
  return i18n_.code() == "ja"
             ? std::to_string(t.date.y) + "年" + std::to_string(t.date.m) + "月" + std::to_string(t.date.d) + "日（" +
                   tr("weekday." + std::to_string(wd)) + "） " + two(t.hour) + ":" + two(t.minute)
             : tr("weekday." + std::to_string(wd)) + " " + std::to_string(t.date.y) + "-" + two(t.date.m) + "-" +
                   two(t.date.d) + " " + two(t.hour) + ":" + two(t.minute);
}

void App::newGame() {
  const auto& m = world_.meta();
  player_ = Player{};
  inside_id_.clear();
  player_.pos = world_.toLocal({m.spawn_lat, m.spawn_lon, 0.0});
  player_.yaw = static_cast<float>(m.spawn_heading * DEG2RAD);
  player_.snapToGround(world_);
  if (settings_.real_time_start) {
    clock_ = rj::sim::GameClock(static_cast<int64_t>(std::time(nullptr)), settings_.time_scale);
  } else {
    const auto today = rj::sim::GameClock(static_cast<int64_t>(std::time(nullptr))).jst().date;
    clock_ = rj::sim::GameClock(rj::sim::GameClock::unixFromJst(today, 8, 0), settings_.time_scale);
  }
  ledger_ = std::make_unique<rj::econ::Ledger>();
  player_account_ = ledger_->open(rj::econ::AccountKind::Person, "player");
  ledger_->endow(player_account_, kStartMoney, clock_.unixUtc(), "初期資金 / starting funds");
  play_seconds_ = 0;
  autosave_timer_ = 0;
  session_t_ = 0;
  in_session_ = true;
  screen_ = Screen::Game;
}

bool App::saveSlot(int slot) {
  if (!in_session_) return false;
  SaveGame s;
  const auto g = world_.toGeodetic(player_.pos);
  s.saved_at = localTimestamp();
  s.lat = g.lat_deg;
  s.lon = g.lon_deg;
  s.h = g.h_ellipsoidal_m;
  s.yaw_deg = player_.yaw * RAD2DEG;
  s.pitch_deg = player_.pitch * RAD2DEG;
  s.fly = player_.fly;
  s.camera_mode = player_.camera_mode;
  s.game_unix = clock_.unixUtc();
  s.money = ledger_ ? ledger_->balance(player_account_) : 0;
  s.play_seconds = play_seconds_;
  s.interior = inside_id_;
  const bool ok = writeSave(slot, s);
  if (slot != kAutosaveSlot) toast(tr(ok ? "save.saved" : "save.failed"));
  return ok;
}

bool App::loadSlot(int slot) {
  auto s = readSave(slot);
  if (!s) {
    toast(tr("save.load_failed"));
    return false;
  }
  player_ = Player{};
  player_.pos = world_.toLocal({s->lat, s->lon, s->h});
  player_.yaw = s->yaw_deg * DEG2RAD;
  player_.pitch = s->pitch_deg * DEG2RAD;
  player_.fly = s->fly;
  player_.camera_mode = s->camera_mode;
  inside_id_.clear();
  if (!s->interior.empty() && world_.forceLoadInterior(s->interior)) inside_id_ = s->interior;
  if (!player_.fly && inside_id_.empty()) player_.snapToGround(world_);
  clock_ = rj::sim::GameClock(s->game_unix, settings_.time_scale);
  ledger_ = std::make_unique<rj::econ::Ledger>();
  player_account_ = ledger_->open(rj::econ::AccountKind::Person, "player");
  if (s->money > 0) ledger_->endow(player_account_, s->money, clock_.unixUtc(), "ロード時残高 / balance at load");
  play_seconds_ = s->play_seconds;
  autosave_timer_ = 0;
  session_t_ = 0;
  in_session_ = true;
  screen_ = Screen::Game;
  toast(tr("save.loaded"));
  return true;
}

void App::endSession() {
  if (in_session_) saveSlot(kAutosaveSlot);
  in_session_ = false;
  inside_id_.clear();
  screen_ = Screen::Title;
}

void App::takeUserScreenshot() {
  Image img = LoadImageFromScreen();
  int size = 0;
  unsigned char* png = ExportImageToMemory(img, ".png", &size);
  UnloadImage(img);
  if (!png) return;
  std::string name = localTimestamp();
  std::replace(name.begin(), name.end(), ':', '-');
  std::replace(name.begin(), name.end(), ' ', '_');
  const auto path = userDir() / "screenshots" / ("RealJapan_" + name + "_" + std::to_string(frame_) + ".png");
  writeFileAtomic(path, std::string(reinterpret_cast<char*>(png), static_cast<size_t>(size)));
  MemFree(png);
  toast(pathToUtf8(path.filename()));
}

void App::applyLaunchOverrides() {
  if (opt_.has_pos) {
    player_.pos = world_.toLocal({opt_.lat, opt_.lon, 0.0});
    player_.snapToGround(world_);
  }
  if (!std::isnan(opt_.yaw_deg)) player_.yaw = opt_.yaw_deg * DEG2RAD;
  if (!std::isnan(opt_.pitch_deg)) player_.pitch = opt_.pitch_deg * DEG2RAD;
  if (opt_.camera_mode >= 0) player_.camera_mode = opt_.camera_mode;
  if (opt_.fly) {
    player_.fly = true;
    player_.pos.z += opt_.alt;
  }
  if (!opt_.weather.empty()) {
    WeatherKind k;
    if (parseWeather(opt_.weather, k)) {
      weather_.set(k, true);
      weather_.setAuto(false);
    }
  }
  if (opt_.dev) settings_.dev_overlay = true;
  rj::sim::CivilDate d;
  int hh, mm;
  if (!opt_.time_jst.empty() && parseJst(opt_.time_jst, d, hh, mm))
    clock_ = rj::sim::GameClock(rj::sim::GameClock::unixFromJst(d, hh, mm), settings_.time_scale);
}

void App::placeRoads() {
  signals_.rebuild(world_);
  if (trains_.loaded()) trains_.place(world_);
  if (ferries_.loaded()) ferries_.place(world_);
  if (aviation_.loaded()) aviation_.place(world_);
  // Large worlds (the island) stream: markings and the walk network cover the area around the player.
  const bool regional = world_.knownCount() > 8;
  const rj::geo::Vec3d c = in_session_ ? player_.pos : rj::geo::Vec3d{0, 0, 0};
  roads_center_ = c;
  if (traffic_.loaded()) {
    traffic_.place(world_);
    traffic_.assignSignalGroups(signals_);
    markings_.build(traffic_, signals_, world_, {c.x, c.y}, regional ? static_cast<double>(settings_.view_distance_m) : 0.0);
    traffic_placed_ = true;
    peds_.buildNav(world_, &traffic_, &markings_);
  } else {
    peds_.buildNav(world_);
  }
}

// ---------------------------------------------------------------------------
void App::update(float dt) {
  ui_.beginFrame();
  if (screen_ == Screen::Boot) return;  // boot happens after the first frame is shown
  if (screen_ == Screen::Fatal) {
    if (!opt_.screenshot.empty() && shot_frames_ < 0) shot_frames_ = 3;  // test aid: capture the error screen
    return;
  }

  if (IsKeyPressed(KEY_F12)) takeUserScreenshot();

  // Stream the world around the current viewpoint.
  rj::geo::Vec3d focus = (in_session_ ? player_.pos : rj::geo::Vec3d{0, 0, 0});
  const bool had_origin = world_.hasOrigin();
  const rj::geo::LocalFrame frame_before = had_origin ? world_.origin().frame() : rj::geo::LocalFrame(rj::geo::Geodetic{});
  if (world_.update(focus, settings_.view_distance_m)) {
    if (in_session_) player_.pos = focus;
    if (had_origin) {  // carry everything held in origin coordinates into the new frame
      const rj::geo::Rigid3d X = world_.origin().frame().transformFrom(frame_before);
      driving_.shiftOrigin(X);
      aviation_.shiftOrigin(X);
      ferries_.shiftOrigin(X);
      jobs_.shiftOrigin(X);
      fish_.bobber = X.apply(fish_.bobber);
      walk_start_ = X.apply(walk_start_);
      TraceLog(LOG_INFO, "RJ: origin rebased");
    }
    facades_.clear();  // facade meshes live in origin coordinates
    traffic_placed_ = false;
  }
  if (in_session_ && screen_ != Screen::Loading) facades_.update(world_, player_.pos, opt_.screenshot.empty() ? 6 : 60);
  if (frame_ % 60 == 0) signals_.rebuild(world_);
  signals_.update(GetTime());
  if (in_session_ && screen_ != Screen::Loading && traffic_.loaded()) {
    if (!traffic_placed_ && world_.residentCount() > 0) placeRoads();
    if (traffic_placed_ && drive_spawn_pending_) {
      drive_spawn_pending_ = false;
      Vehicle v;
      v.type = VehicleType::Sedan;
      v.color[0] = 0.55f, v.color[1] = 0.05f, v.color[2] = 0.06f;
      double hd = player_.yaw;
      v.pos = player_.pos;
      traffic_.nearestLane(player_.pos, player_.yaw, v.pos, hd);
      v.yaw = static_cast<float>(hd);
      if (auto h = world_.roadHeight(v.pos.x, v.pos.y)) v.pos.z = *h;
      driving_.enter(v);
      player_.pos = v.pos;
    }
    // Streamed worlds: rebuild markings / walk network once the player has moved on and cells settled.
    if (traffic_placed_ && world_.knownCount() > 8 && world_.pendingJobs() == 0 &&
        std::hypot(player_.pos.x - roads_center_.x, player_.pos.y - roads_center_.y) > 600.0)
      placeRoads();
    if (traffic_placed_ && screen_ != Screen::Pause && screen_ != Screen::Settings) {
      traffic_.setObstacle(driving_.hasCar(), driving_.car().pos);
      traffic_.update(dt, world_, signals_, player_.pos, clock_.jst().hour);
    }
    if (frame_ % 30 == 0 && std::getenv("RJ_DEBUG")) {
      double dmin = 1e9;
      const Vehicle* nv = nullptr;
      for (const auto& v : traffic_.vehicles()) {
        const double d = std::hypot(v.pos.x - player_.pos.x, v.pos.y - player_.pos.y);
        if (d < dmin) {
          dmin = d;
          nv = &v;
        }
      }
      TraceLog(LOG_DEBUG, "RJ: traffic %zu vehicles, nearest %.0f m, edges %zu, %.1f km", traffic_.vehicles().size(), dmin,
               traffic_.edgeCount(), traffic_.networkKm());
      if (nv)
        TraceLog(LOG_DEBUG, "RJ: nearest vehicle dx %.1f dy %.1f dz %.2f yaw %.0f type %d v %.1f", nv->pos.x - player_.pos.x,
                 nv->pos.y - player_.pos.y, nv->pos.z - player_.pos.z, nv->yaw * RAD2DEG, static_cast<int>(nv->type), nv->v);
    }
  }

  if (screen_ == Screen::Loading) {
    // Small slices load completely; streamed worlds start once the cells around the spawn are in.
    if (world_.residentCount() >= world_.knownCount() ||
        (world_.pendingJobs() == 0 && world_.residentCount() > 0 && frame_ > (world_.knownCount() > 8 ? 20 : 600))) {
      player_.snapToGround(world_);
      placeRoads();
      screen_ = Screen::Title;
      if (opt_.selftest) {
        runSelfTest();
        return;
      }
      applyLaunchOverrides();
      if (!opt_.state.empty()) {
        const std::string st = opt_.state;
        if (st == "title") {
        } else if (st == "settings") {
          screen_ = Screen::Settings;
        } else if (st == "credits") {
          screen_ = Screen::Credits;
        } else {
          newGame();
          applyLaunchOverrides();
          if (st == "pause") screen_ = Screen::Pause;
          if (st == "interior" && !world_.meta().interiors.empty()) {
            const auto& im = world_.meta().interiors.front();
            if (world_.forceLoadInterior(im.id)) {
              const Interior* in = world_.interior(im.id);
              const Interior::Entrance* best = nullptr;
              const auto& ents = in->entrances();
              if (opt_.entrance >= 0 && opt_.entrance < static_cast<int>(ents.size())) best = &ents[opt_.entrance];
              for (const auto& e : ents)
                if (opt_.entrance < 0 &&
                    (!best || std::hypot(e.street.x - player_.pos.x, e.street.y - player_.pos.y) <
                                  std::hypot(best->street.x - player_.pos.x, best->street.y - player_.pos.y)))
                  best = &e;
              if (best) {
                player_.pos = {best->inside.x, best->inside.y, best->inside.z + 0.05};
                inside_id_ = im.id;
              }
            }
          }
          player_.auto_forward_s = opt_.autowalk;
          walk_start_ = player_.pos;
          // "--walk 0:12,270:8": walk each leg (compass yaw, seconds) in turn after any --autowalk.
          for (size_t p0 = 0; p0 < opt_.walk.size();) {
            size_t p1 = opt_.walk.find(',', p0);
            if (p1 == std::string::npos) p1 = opt_.walk.size();
            float yd = 0, sec = 0;
            if (std::sscanf(opt_.walk.substr(p0, p1 - p0).c_str(), "%f:%f", &yd, &sec) == 2) walk_legs_.push_back({yd, sec});
            p0 = p1 + 1;
          }
          if (st == "drive") {
            // test aid: a sedan on the nearest lane (placed once the road graph is), heading near --yaw;
            // --drive "throttle:steer:seconds,..."
            drive_spawn_pending_ = true;
            drive_look_pitch_ = -0.08f;
            drive_first_person_ = std::getenv("RJ_DRIVE_FP") != nullptr;  // test aid: driver's seat view
            if (const char* lk = std::getenv("RJ_DRIVE_LOOK")) drive_look_yaw_ = static_cast<float>(std::atof(lk)) * DEG2RAD;
            for (size_t p0 = 0; p0 < opt_.drive.size();) {
              size_t p1 = opt_.drive.find(',', p0);
              if (p1 == std::string::npos) p1 = opt_.drive.size();
              DriveLeg L{};
              if (std::sscanf(opt_.drive.substr(p0, p1 - p0).c_str(), "%f:%f:%f", &L.throttle, &L.steer, &L.seconds) == 3)
                drive_legs_.push_back(L);
              p0 = p1 + 1;
            }
          }
          if ((st == "ride" || st == "platform" || st == "trainjob") && trains_.loaded() && opt_.station >= 0 &&
              opt_.station < static_cast<int>(trains_.stations().size())) {
            ride_test_t_ = st == "platform" ? -1.0f : 0.0f;  // platform: just stand there and look
            ride_place_pending_ = true;
          }
          if (st == "fly" && aviation_.loaded()) {  // test aid: in the light aircraft lined up on the runway
            fly_test_pending_ = true;
            for (size_t p0 = 0; p0 < opt_.fly_script.size();) {
              size_t p1 = opt_.fly_script.find(',', p0);
              if (p1 == std::string::npos) p1 = opt_.fly_script.size();
              FlyLeg L{};
              if (std::sscanf(opt_.fly_script.substr(p0, p1 - p0).c_str(), "%f:%f:%f:%f", &L.throttle, &L.elevator, &L.aileron, &L.seconds) == 4)
                fly_legs_.push_back(L);
              p0 = p1 + 1;
            }
          }
          if (st == "jet" && aviation_.loaded()) {  // test aid: at the terminal, board the next flight, ride
            ride_test_t_ = 0.0f;
            const Airport& ap = aviation_.airport();
            const rj::geo::Vec3d nx{ap.twy_a.x - ap.rwy_a.x, ap.twy_a.y - ap.rwy_a.y, 0};
            const double l = std::max(1.0, std::hypot(nx.x, nx.y));
            player_.pos = {ap.terminal.x + nx.x / l * 140.0, ap.terminal.y + nx.y / l * 140.0, ap.terminal.z};
            player_.snapToGround(world_);
          }
          if ((st == "ferry" || st == "ferryview") && ferries_.loaded()) {  // test aid: beside the ship at pier --station (board, ride)
            ferry_test_pending_ = true;
            ride_test_t_ = st == "ferry" ? 0.0f : -1.0f;
          }
          if (st.rfind("phone", 0) == 0) {
            screen_ = Screen::Phone;
            if (st == "phone:map") phone_app_ = PhoneApp::Map;
            if (st == "phone:town") phone_app_ = PhoneApp::Town;
            if (st == "phone:clock") phone_app_ = PhoneApp::Clock;
            if (st == "phone:wallet") phone_app_ = PhoneApp::Wallet;
            if (st == "phone:work") phone_app_ = PhoneApp::Work;
            if (st == "phone:hobby") phone_app_ = PhoneApp::Hobby;
          }
        }
        if (!opt_.screenshot.empty()) shot_frames_ = opt_.frames;
      } else if (!opt_.screenshot.empty()) {
        shot_frames_ = opt_.frames;
      }
    }
    return;
  }

  const bool want_lock = (screen_ == Screen::Game) && !till_.on;
  if (want_lock != cursor_locked_) {
    if (want_lock) DisableCursor();
    else EnableCursor();
    cursor_locked_ = want_lock;
  }

  switch (screen_) {
    case Screen::Title:
      title_t_ += dt;
      if (!in_session_ && opt_.state.empty()) clock_.advanceReal(dt / std::max(1, settings_.time_scale));  // real time
      break;
    case Screen::Game:
    case Screen::Phone: {
      clock_.setTimeScale(settings_.time_scale);
      clock_.advanceReal(dt);
      play_seconds_ += dt;
      session_t_ += dt;
      autosave_timer_ += dt;
      if (autosave_timer_ > kAutosaveInterval) {
        autosave_timer_ = 0;
        saveSlot(kAutosaveSlot);
      }
      if (!inside_id_.empty() && !insideInterior()) world_.forceLoadInterior(inside_id_);
      if (player_.auto_forward_s <= 0.0f && !walk_legs_.empty()) {
        player_.yaw = walk_legs_.front().first * DEG2RAD;
        player_.auto_forward_s = walk_legs_.front().second;
        walk_legs_.erase(walk_legs_.begin());
      }
      const Interior* nearby = nullptr;  // outside: the interior whose stairwell openings are closest
      if (inside_id_.empty()) {
        double best = 25.0;
        for (const auto& [id, in] : world_.interiors())
          if (const double d = in->distanceToOpening(player_.pos.x, player_.pos.y); d < best) {
            best = d;
            nearby = in.get();
          }
      }
      if (trains_.loaded()) trains_.update(std::min(dt, 0.1f));
      for (int k = 0; k < opt_.sim_speed; ++k) {
        if (k > 0 && trains_.loaded()) trains_.update(std::min(dt, 0.1f));
        if (ferries_.loaded()) ferries_.update(std::min(dt, 0.1f), render_time_ + k * 0.1f, lighting_.wind);
        if (aviation_.loaded()) aviation_.update(std::min(dt, 0.1f), world_);
      }
      if (ride_place_pending_ && traffic_placed_) {
        ride_place_pending_ = false;
        placeRideTest();
      }
      if (fly_test_pending_ && traffic_placed_) {
        fly_test_pending_ = false;
        const Airport& ap = aviation_.airport();
        const double dx = ap.rwy_b.x - ap.rwy_a.x, dy = ap.rwy_b.y - ap.rwy_a.y, l = std::max(1.0, std::hypot(dx, dy));
        aviation_.plane().reset({ap.rwy_a.x + dx / l * 120.0, ap.rwy_a.y + dy / l * 120.0, ap.rwy_a.z}, std::atan2(dx, dy) * RAD2DEG, world_);
        flying_ = true;
        fly_cockpit_ = std::getenv("RJ_FLY_COCKPIT") != nullptr;
        plane_in_ = PlaneControls{};
      }
      if (ferry_test_pending_ && traffic_placed_) {
        ferry_test_pending_ = false;
        const int pi = std::clamp(opt_.station, 0, static_cast<int>(ferries_.piers().size()) - 1);
        if (const Ferry* f = ferries_.dockedAt(pi)) {
          player_.pos = ferries_.gangwayPier(*f);
          player_.snapToGround(world_);
          const rj::geo::Vec3d fp = f->pos;
          player_.yaw = static_cast<float>(std::atan2(fp.x - player_.pos.x, fp.y - player_.pos.y));
          if (opt_.state == "ferryview") {  // stand back along the pier to see the whole ship
            const double fx = std::sin(f->yaw), fy = std::cos(f->yaw);
            player_.pos = {player_.pos.x + fx * 45.0, player_.pos.y + fy * 45.0, player_.pos.z};
            player_.snapToGround(world_);
            player_.yaw = static_cast<float>(std::atan2(fp.x - player_.pos.x, fp.y - player_.pos.y));
          }
        }
      }
      if (ride_ferry_ >= 0) {
        // Aboard: walk about the open deck (WASD), look around with the mouse; the ship carries you.
        if (const Ferry* f = ferries_.ship(ride_ferry_)) {
          const ShipClass& C = Ferries::shipClass(f->cls);
          if (screen_ == Screen::Game) {
            const Vector2 md = GetMouseDelta();
            const float sens = 0.0022f * settings_.mouse_sensitivity;
            ferry_look_yaw_ = std::remainder(ferry_look_yaw_ + md.x * sens, 2.0f * PI);
            player_.pitch = std::clamp(player_.pitch + (settings_.invert_y ? md.y : -md.y) * sens, -1.4f, 1.4f);
            const float fwd = (IsKeyDown(KEY_W) ? 1.0f : 0.0f) - (IsKeyDown(KEY_S) ? 1.0f : 0.0f);
            const float side = (IsKeyDown(KEY_D) ? 1.0f : 0.0f) - (IsKeyDown(KEY_A) ? 1.0f : 0.0f);
            const float spd = (IsKeyDown(KEY_LEFT_SHIFT) ? 3.2f : 1.4f) * dt;
            const double a = ferry_look_yaw_;
            ferry_x_ += (std::sin(a) * fwd + std::cos(a) * side) * spd;
            ferry_y_ += (std::cos(a) * fwd - std::sin(a) * side) * spd;
          }
          ferry_x_ = std::clamp(ferry_x_, -static_cast<double>(C.deck_x), static_cast<double>(C.deck_x));
          ferry_y_ = std::clamp(ferry_y_, C.deck_y0 + 0.5, C.deck_y1 - 0.5);
          const double hx = C.house_x + 0.35, hy0 = C.house_y0 - 0.35, hy1 = C.house_y1 + 0.35;
          if (std::fabs(ferry_x_) < hx && ferry_y_ > hy0 && ferry_y_ < hy1) {  // round the deckhouse
            const double dx = hx - std::fabs(ferry_x_), d0 = ferry_y_ - hy0, d1 = hy1 - ferry_y_;
            if (dx <= d0 && dx <= d1) ferry_x_ = ferry_x_ >= 0 ? hx : -hx;
            else if (d0 < d1) ferry_y_ = hy0;
            else ferry_y_ = hy1;
          }
          player_.pos = ferries_.toWorld(*f, ferry_x_, ferry_y_, C.deck_z);
          player_.yaw = f->yaw + ferry_look_yaw_;
          player_.cam_z = player_.pos.z;
          player_.cam_z_init = true;
          player_.bob_amount = 0.0f;
          if (f->phase == Ferry::Phase::Offmap) ferries_.fastForwardOffmap(f->id);
        } else {
          ride_ferry_ = -1;
        }
      } else if (ride_jet_ >= 0) {
        // Scheduled flight: seated at the window, look around with the mouse.
        if (screen_ == Screen::Game) {
          const Vector2 md = GetMouseDelta();
          const float sens = 0.0022f * settings_.mouse_sensitivity;
          jet_look_yaw_ = std::clamp(jet_look_yaw_ + md.x * sens, -2.4f, 2.4f);
          jet_look_pitch_ = std::clamp(jet_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -1.1f, 1.1f);
        }
        if (const Airliner* a = aviation_.airliner(ride_jet_)) {
          if (a->phase != Airliner::Phase::AtStand) jet_flown_ = true;
          if (a->phase == Airliner::Phase::Offmap) aviation_.fastForwardOffmap(a->id);
          else player_.pos = {a->pos.x, a->pos.y, a->pos.z - 2.4};
        } else {
          ride_jet_ = -1;
        }
      } else if (flying_) {
        // Light aircraft: W/S pitch, A/D roll, Q/E rudder, Shift/Ctrl throttle, F/R flaps, Space brakes.
        LightPlane& pl = aviation_.plane();
        const bool stopped = pl.onGround() && pl.airspeed() < 1.0 && !pl.crashed();
        if (screen_ == Screen::Game) {
          const Vector2 md = GetMouseDelta();
          const float sens = 0.0022f * settings_.mouse_sensitivity;
          fly_look_yaw_ = std::clamp(fly_look_yaw_ + md.x * sens, -2.8f, 2.8f);
          fly_look_pitch_ = std::clamp(fly_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -1.0f, 1.0f);
          if (IsKeyPressed(KEY_V)) fly_cockpit_ = !fly_cockpit_;
          if (IsKeyPressed(KEY_E) && stopped && fly_legs_.empty()) {
            const rj::geo::Vec3d r = pl.right();
            player_.pos = {pl.pos().x - r.x * 1.7, pl.pos().y - r.y * 1.7, pl.pos().z};
            player_.snapToGround(world_);
            player_.yaw = static_cast<float>(pl.heading() * DEG2RAD);
            flying_ = false;
            toast(tr("fly.left"));
          } else {
            auto key = [](int k) { return IsKeyDown(k) ? 1.0f : 0.0f; };
            plane_in_.elevator = key(KEY_S) - key(KEY_W) + key(KEY_DOWN) - key(KEY_UP);
            plane_in_.aileron = key(KEY_D) - key(KEY_A) + key(KEY_RIGHT) - key(KEY_LEFT);
            plane_in_.rudder = (stopped ? 0.0f : key(KEY_E)) - key(KEY_Q);
            plane_in_.throttle = std::clamp(plane_in_.throttle + (key(KEY_LEFT_SHIFT) - key(KEY_LEFT_CONTROL)) * 0.6f * dt, 0.0f, 1.0f);
            if (IsKeyPressed(KEY_F)) plane_in_.flaps = std::min(3, plane_in_.flaps + 1);
            if (IsKeyPressed(KEY_R)) plane_in_.flaps = std::max(0, plane_in_.flaps - 1);
            plane_in_.brake = IsKeyDown(KEY_SPACE);
          }
        }
        if (!fly_legs_.empty()) {  // test aid
          auto& L = fly_legs_.front();
          plane_in_.throttle = L.throttle;
          plane_in_.elevator = L.elevator;
          plane_in_.aileron = L.aileron;
          plane_in_.brake = false;
          if ((L.seconds -= std::min(dt, 0.05f) * static_cast<float>(opt_.sim_speed)) <= 0) fly_legs_.erase(fly_legs_.begin());
          if (frame_ % 20 == 0)
            TraceLog(LOG_INFO, "RJ: fly v %.1f kt alt %.0f m vs %.1f pitch %.1f roll %.1f aoa %.1f ground %d", pl.airspeed() * 1.944,
                     pl.pos().z - aviation_.airport().rwy_a.z, pl.verticalSpeed(), pl.pitchDeg(), pl.rollDeg(), pl.alpha() * RAD2DEG, pl.onGround());
        }
        if (flying_) {
          for (int k = 0; k < opt_.sim_speed; ++k) pl.update(dt, world_, plane_in_, weather_.now().wind_ms);
          player_.pos = pl.pos();
          if (pl.crashed()) {
            if (crash_t_ < 0) {
              crash_t_ = 0;
              toast(tr("fly.crashed"));
            }
            crash_t_ += dt;
            if (crash_t_ > 3.0f) {
              crash_t_ = -1;
              flying_ = false;
              aviation_.resetPlane(world_);
              const rj::geo::Vec3d r = aviation_.plane().right();
              player_.pos = {aviation_.plane().pos().x - r.x * 2.0, aviation_.plane().pos().y - r.y * 2.0, aviation_.plane().pos().z};
              player_.snapToGround(world_);
            }
          }
        }
      } else if (ride_train_ >= 0) {
        // Riding: the view turns with the car; look around with the mouse.
        if (screen_ == Screen::Game) {
          const Vector2 md = GetMouseDelta();
          const float sens = 0.0022f * settings_.mouse_sensitivity;
          ride_look_yaw_ = std::clamp(ride_look_yaw_ + md.x * sens, -2.6f, 2.6f);
          ride_look_pitch_ = std::clamp(ride_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -1.2f, 1.2f);
        }
        if (const Train* t = trains_.train(ride_train_)) {
          rj::geo::Vec3d p;
          float yaw, pitch;
          trains_.carPose(*t, ride_car_, p, yaw, pitch);
          player_.pos = {p.x, p.y, p.z + 1.2};
          player_.yaw = yaw + ride_look_yaw_;
        }
      } else if (driving_.active()) {
        // Driving: WASD / arrows, Space handbrake, V toggles chase / driver's seat view.
        if (screen_ == Screen::Game) {
          const Vector2 md = GetMouseDelta();
          const float sens = 0.0022f * settings_.mouse_sensitivity;
          drive_look_yaw_ = std::clamp(drive_look_yaw_ + md.x * sens, -2.8f, 2.8f);
          drive_look_pitch_ = std::clamp(drive_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -0.9f, 0.9f);
          if (std::fabs(md.x) < 0.5f && std::fabs(driving_.car().v) > 3.0 && !std::getenv("RJ_DRIVE_LOOK"))  // camera settles behind
            drive_look_yaw_ -= drive_look_yaw_ * std::min(1.0f, dt * 1.5f);
          if (IsKeyPressed(KEY_V)) drive_first_person_ = !drive_first_person_;
        }
        DriveInput din = screen_ == Screen::Game ? readDriveInput() : DriveInput{};
        if (!drive_legs_.empty()) {  // test aid
          auto& L = drive_legs_.front();
          din.throttle = std::max(0.0f, L.throttle);
          din.brake = std::max(0.0f, -L.throttle);
          din.steer = L.steer;
          if ((L.seconds -= std::min(dt, 0.05f)) <= 0) drive_legs_.erase(drive_legs_.begin());  // same step as the car
        }
        driving_.update(dt, world_, traffic_, din, weather_.wetness());
        if (frame_ % 15 == 0 && (!drive_legs_.empty() || std::getenv("RJ_DEBUG")))
          TraceLog(LOG_INFO, "RJ: drive pos %.1f %.1f z %.2f v %.1f yaw %.0f pitch %.1f", driving_.car().pos.x, driving_.car().pos.y,
                   driving_.car().pos.z, driving_.car().v, driving_.car().yaw * RAD2DEG, driving_.car().pitch * RAD2DEG);
        player_.pos = driving_.car().pos;
        player_.yaw = driving_.car().yaw + drive_look_yaw_;
      } else {
        player_.update(dt, world_, settings_, screen_ == Screen::Game && !till_.on && worship_.stage == 0 && fish_.stage == 0, insideInterior(), nearby);
      }
      if (!inside_id_.empty() && player_.left_interior) {
        // Walked up the stairs and out onto the pavement.
        inside_id_.clear();
        toast(tr("interior.exited"));
        TraceLog(LOG_INFO, "RJ: walked out onto the street at z %.2f", player_.pos.z);
      } else if (nearby && !player_.fly && nearby->overOpening(player_.pos.x, player_.pos.y)) {
        // Walked into a real stairwell opening: continue on the verified interior's stairs.
        if (auto f = nearby->floorBelow(player_.pos.x, player_.pos.y, player_.pos.z + 0.3, 12.0)) {
          if (player_.pos.z - *f > 0.6) player_.grounded = false;
          inside_id_ = nearby->id();
          toast(i18n_.f("interior.entered", {{"name", nearby->name()}}));
          TraceLog(LOG_INFO, "RJ: walked into stairwell of %s (floor %.2f m below)", nearby->id().c_str(), player_.pos.z - *f);
        }
      }
      if (player_.auto_forward_s > 0.0f && (frame_ % 30) == 0)
        TraceLog(LOG_DEBUG, "RJ: walk pos %.2f %.2f z %.2f grounded %d (from start %.2f %.2f %.2f)", player_.pos.x, player_.pos.y,
                 player_.pos.z, player_.grounded, player_.pos.x - walk_start_.x, player_.pos.y - walk_start_.y,
                 player_.pos.z - walk_start_.z);
      {
        // Fewer people out in bad weather (game assumption).
        const WeatherKind wk = weather_.kind();
        const float crowd = wk == WeatherKind::Thunder ? 0.45f : wk == WeatherKind::HeavyRain ? 0.55f
                            : wk == WeatherKind::Rain ? 0.72f : wk == WeatherKind::LightRain ? 0.85f : 1.0f;
        peds_.setHazard(driving_.active(), driving_.car().pos, driving_.car().yaw, std::fabs(driving_.car().v));
        peds_.update(town_, world_, signals_, clock_.jst(), player_.pos, dt, crowd);
      }
      if (screen_ == Screen::Game) {
        updateInteriorAction();
        updateTransportActions();
        updateFerryActions();
        updateAviationActions();
        updateDriveActions();
        updateRideTest();
        updateActivities(dt);
        if (inside_id_.empty()) {
          hover_ = world_.pick(player_.eyeEnu(), player_.forwardEnu(), 300.0);
          hover_walker_ =
              peds_.pick(player_.eyeEnu(), player_.forwardEnu(), hover_ ? std::min(40.0, hover_->distance) : 40.0);
        } else {
          hover_.reset();
          hover_walker_ = nullptr;
        }
        if (IsKeyPressed(KEY_ESCAPE) && photo_mode_) {
          photo_mode_ = false;
        } else if (IsKeyPressed(KEY_ESCAPE) && till_.on) {
          till_.on = false;
        } else if (IsKeyPressed(KEY_ESCAPE) && fish_.stage == 0) {
          screen_ = Screen::Pause;
        }
        if (IsKeyPressed(KEY_TAB)) {
          screen_ = Screen::Phone;
          phone_app_ = PhoneApp::Home;
        }
        if (IsKeyPressed(KEY_F5)) saveSlot(1);
        if (IsKeyPressed(KEY_F3)) {
          settings_.dev_overlay = !settings_.dev_overlay;
          saveSettings();
        }
      } else {
        hover_.reset();
        hover_walker_ = nullptr;
        if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_ESCAPE)) screen_ = Screen::Game;
        const float wheel = GetMouseWheelMove();
        if (phone_app_ == PhoneApp::Map && wheel != 0.0f)
          map_half_extent_ = std::clamp(map_half_extent_ * (wheel > 0 ? 0.8 : 1.25), 80.0, 1600.0);
      }
      break;
    }
    case Screen::Pause:
      if (IsKeyPressed(KEY_ESCAPE)) screen_ = Screen::Game;
      break;
    case Screen::Settings:
      if (IsKeyPressed(KEY_ESCAPE)) screen_ = settings_return_;
      break;
    case Screen::Slots:
      if (IsKeyPressed(KEY_ESCAPE)) screen_ = slots_return_;
      break;
    case Screen::Credits:
      if (IsKeyPressed(KEY_ESCAPE)) screen_ = Screen::Title;
      break;
    default:
      break;
  }
  const auto t = clock_.jst();
  (void)t;
  const auto g = world_.toGeodetic(in_session_ ? player_.pos : rj::geo::Vec3d{0, 0, 0});
  const auto sun = rj::env::sunPosition(clock_.unixUtc(), g.lat_deg, g.lon_deg);
  const int64_t now_unix = clock_.unixUtc();
  const double game_dt = weather_prev_unix_ ? static_cast<double>(now_unix - weather_prev_unix_) : 0.0;
  weather_prev_unix_ = now_unix;
  render_time_ += dt;
  weather_.update(game_dt, dt, static_cast<float>(sun.elevation_deg));
  lighting_ = computeLighting(static_cast<float>(sun.elevation_deg), static_cast<float>(sun.azimuth_deg), weather_.now(),
                              weather_.wetness(), weather_.lightning(), Vector2{weather_.cloudOffsetX(), weather_.cloudOffsetY()});
  {
    // Lit-window fractions by local time (office / residential / shop) — game-side assumption.
    const auto jt = clock_.jst();
    const float h = static_cast<float>(jt.hour) + static_cast<float>(jt.minute) / 60.0f;
    const int wd = rj::sim::weekday(jt.date);
    const bool weekend = wd == 0 || wd == 6 || rj::sim::isHoliday(jt.date);
    auto band = [&](float a, float b) { return h >= a && h < b; };
    const float office = band(8, 19) ? (weekend ? 0.3f : 0.85f) : band(19, 22) ? (weekend ? 0.12f : 0.45f) : 0.07f;
    const float resid = band(6, 8) ? 0.35f : band(8, 17) ? 0.15f : band(17, 24) ? 0.6f : 0.06f;
    const float shop = band(10, 21) ? 0.9f : band(21, 24) ? 0.4f : band(7, 10) ? 0.3f : 0.1f;
    lighting_.occupancy = Vector3{office, resid, shop};
  }
  lighting_.wind = std::clamp(weather_.now().wind_ms / 8.0f, 0.15f, 1.6f);
  // Keep the end of the drawn world inside the haze.
  lighting_.fog_density = std::max(lighting_.fog_density, 1.1f / (static_cast<float>(settings_.view_distance_m) * 1.6f + 500.0f));
  // Underground: blend to artificial light as the eye drops below the street surface, so stairwells
  // near an entrance still see daylight and the sky.
  underground_ = 0.0f;
  if (in_session_ && insideInterior() && screen_ != Screen::Title) {
    const rj::geo::Vec3d eye = player_.eyeEnu();
    const double street = world_.terrainHeight(eye.x, eye.y).value_or(eye.z + 10.0);
    underground_ = static_cast<float>(std::clamp((street - eye.z + 0.3) / 2.5, 0.0, 1.0));
    lighting_ = lerpLighting(lighting_, indoorLighting(), underground_);
  }
}

void App::updateInteriorAction() {
  prompt_.clear();
  if (driving_.active()) return;
  const bool press = IsKeyPressed(KEY_E);
  if (const Interior* in = insideInterior()) {
    for (const auto& e : in->entrances()) {
      if (std::hypot(player_.pos.x - e.inside.x, player_.pos.y - e.inside.y) < 4.0 && std::fabs(player_.pos.z - e.inside.z) < 2.5) {
        prompt_ = tr("interior.exit");
        if (press) {
          player_.pos = e.street;
          player_.snapToGround(world_);
          inside_id_.clear();
          toast(tr("interior.exited"));
        }
        return;
      }
    }
    return;
  }
  for (const auto& [id, in] : world_.interiors())
    for (const auto& e : in->entrances()) {
      if (std::hypot(player_.pos.x - e.street.x, player_.pos.y - e.street.y) < 4.0 && std::fabs(player_.pos.z - e.street.z) < 2.5) {
        prompt_ = i18n_.f("interior.enter", {{"name", in->name()}});
        if (press) {
          player_.pos = {e.inside.x, e.inside.y, e.inside.z + 0.05};
          player_.vel_z = 0;
          player_.fly = false;
          inside_id_ = id;
          toast(i18n_.f("interior.entered", {{"name", in->name()}}));
        }
        return;
      }
    }
}

std::string App::lineName(int line) const {
  if (line < 0 || line >= static_cast<int>(trains_.lines().size())) return "";
  switch (trains_.lines()[static_cast<size_t>(line)].kind) {
    case LineKind::Loop: return tr("rail.line.loop");
    case LineKind::Branch: return tr("rail.line.branch");
    default: return tr("rail.line.shinkansen");
  }
}

Camera3D App::rideCamera() const {
  Camera3D c{};
  const Train* t = trains_.train(ride_train_);
  if (!t) return player_.camera(settings_.fov);
  rj::geo::Vec3d p;
  float yaw, pitch;
  trains_.carPose(*t, ride_car_, p, yaw, pitch);
  // seated: Shinkansen window seat (left, facing forward), commuter long bench seat (left, facing across)
  const double fx = std::sin(yaw), fy = std::cos(yaw), rx = fy, ry = -fx;
  double sx, sy, sz;
  if (trains_.lines()[static_cast<size_t>(t->line)].kind == LineKind::Shinkansen) {
    const float row = kShinkansenSeatRow0 + std::round(-kShinkansenSeatRow0 / kShinkansenSeatPitch) * kShinkansenSeatPitch;
    sx = -1.28;
    sy = row - 0.12;
    sz = kShinkansenFloorZ + 1.12;
  } else {
    sx = -1.0;
    sy = 0.0;
    sz = kCommuterFloorZ + 1.1;
  }
  const rj::geo::Vec3d eye{p.x + rx * sx + fx * sy, p.y + ry * sx + fy * sy, p.z + sz + std::tan(pitch) * sy};
  const float y = yaw + ride_look_yaw_, pt = pitch + ride_look_pitch_;
  const rj::geo::Vec3d f{std::sin(y) * std::cos(pt), std::cos(y) * std::cos(pt), std::sin(pt)};
  c.position = enuToRl(eye);
  c.target = enuToRl({eye.x + f.x, eye.y + f.y, eye.z + f.z});
  c.up = {0, 1, 0};
  c.fovy = settings_.fov;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

void App::placeRideTest() {
  // test aid: stand on the platform beside the next train to stop here (boarding is automatic)
  const Station& sn = trains_.stations()[static_cast<size_t>(opt_.station)];
  const double hd = sn.heading * DEG2RAD;
  const double sx = std::cos(hd), sy = -std::sin(hd);
  const auto kind = trains_.lines()[static_cast<size_t>(sn.line)].kind;
  double side = 1.0;  // the platform of the first train that stops here (dir +1: left of the heading)
  for (const auto& t : trains_.trains())
    if (t.line == sn.line) {
      side = t.dir > 0 ? -1.0 : 1.0;
      if (t.at_station == opt_.station) break;
    }
  const double off = Trains::platformOffset(kind) - 1.4;
  player_.pos = {sn.pos.x + sx * side * off, sn.pos.y + sy * side * off, sn.pos.z + 0.05};
  player_.yaw = static_cast<float>(hd + (side > 0 ? -PI / 2 : PI / 2));
  if (opt_.state == "platform") {  // on the platform just ahead of the train's nose, looking back at it
    for (const auto& t : trains_.trains()) {
      if (t.line != sn.line || t.at_station != opt_.station) continue;
      rj::geo::Vec3d p;
      float yaw, pitch;
      trains_.carPose(t, 0, p, yaw, pitch);
      const double fx = std::sin(yaw), fy = std::cos(yaw), lx = -fy, ly = fx;  // train forward / left (platform side)
      const double ahead = t.car_len * 0.5 + 6.0, lat = Trains::platformOffset(kind) - Trains::trackOffset(kind) - 1.2;
      player_.pos = {p.x + fx * ahead + lx * lat, p.y + fy * ahead + ly * lat, sn.pos.z + 0.05};
      player_.yaw = static_cast<float>(yaw + PI - 0.28);
      player_.pitch = -0.06f;
      break;
    }
  }
  player_.vel_z = 0;
  player_.fly = false;
  TraceLog(LOG_INFO, "RJ: ride test on %s platform (side %.0f) at %.1f %.1f %.1f", sn.name.c_str(), side, player_.pos.x, player_.pos.y, player_.pos.z);
}

void App::updateRideTest() {
  if (ride_test_t_ < 0.0f || ride_test_done_) return;
  if (opt_.state == "trainjob" && ride_train_ >= 0 && drive_train_ < 0) {  // take the controls once aboard
    if (const Train* t = trains_.train(ride_train_); t && t->at_station >= 0) startTrainDriving();
  }
  if (frame_ % 60 == 0)
    TraceLog(LOG_INFO, "RJ: ride test t %.1f train %d ferry %d jet %d prompt '%s' pos %.1f %.1f %.1f", ride_test_t_, ride_train_, ride_ferry_, ride_jet_,
             prompt_.c_str(), player_.pos.x, player_.pos.y, player_.pos.z);
  if (ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0) {
    ride_test_t_ += std::min(GetFrameTime(), 0.1f) * static_cast<float>(opt_.sim_speed);
    if (!opt_.alight && ride_test_t_ >= opt_.ride) ride_test_done_ = true;
  } else if (ride_test_t_ > 0.0f) {
    ride_test_t_ += std::min(GetFrameTime(), 0.1f);  // alighted: settle for a moment
    if (ride_test_t_ > opt_.ride + 1.0f) ride_test_done_ = true;
  }
}

bool App::autoKeyE() const {
  if (ride_test_t_ < 0.0f || ride_test_done_ || ferry_test_pending_ || ride_place_pending_) return false;
  if (ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0) return frame_ % 20 == 0;  // board
  return opt_.alight && ride_test_t_ >= opt_.ride && frame_ % 20 == 0;  // alight at the next stop
}

bool App::scriptBusy() const {
  if (player_.auto_forward_s > 0.0f || !walk_legs_.empty() || !drive_legs_.empty() || drive_spawn_pending_ || ferry_test_pending_ || ride_place_pending_ ||
      fly_test_pending_ || !fly_legs_.empty())
    return true;
  return ride_test_t_ >= 0.0f && !ride_test_done_;
}

std::string App::pierName(int pier) const { return tr("ferry.pier." + std::to_string(pier)); }

Camera3D App::jetCamera() const {
  Camera3D c{};
  const Airliner* a = aviation_.airliner(ride_jet_);
  if (!a) return player_.camera(settings_.fov);
  const Vector3 p = enuToRl(a->pos);
  const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a->roll), MatrixRotateX(a->pitch)), MatrixRotateY(-a->yaw)),
                                  MatrixTranslate(p.x, p.y, p.z));
  const Vector3 eye = Vector3Transform({kJetSeat[0], kJetSeat[2], -kJetSeat[1]}, M);
  const float ly = jet_look_yaw_, lp = jet_look_pitch_;
  const Vector3 dir_m{std::sin(ly) * std::cos(lp), std::sin(lp), -std::cos(ly) * std::cos(lp)};  // model -> raylib axes
  const Vector3 tgt = Vector3Transform(Vector3Add({kJetSeat[0], kJetSeat[2], -kJetSeat[1]}, dir_m), M);
  const Vector3 up = Vector3Subtract(Vector3Transform({kJetSeat[0], kJetSeat[2] + 1.0f, -kJetSeat[1]}, M), eye);
  c.position = eye;
  c.target = tgt;
  c.up = Vector3Normalize(up);
  c.fovy = settings_.fov;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

void App::updateAviationActions() {
  if (!aviation_.loaded() || !inside_id_.empty() || driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0) return;
  const bool e = IsKeyPressed(KEY_E) || autoKeyE();
  const Airport& ap = aviation_.airport();
  const rj::geo::Vec3d nxv{ap.twy_a.x - ap.rwy_a.x, ap.twy_a.y - ap.rwy_a.y, 0};
  const double nl = std::max(1.0, std::hypot(nxv.x, nxv.y));
  const rj::geo::Vec3d landside{ap.terminal.x + nxv.x / nl * 140.0, ap.terminal.y + nxv.y / nl * 140.0, ap.terminal.z};
  if (ride_jet_ >= 0) {
    const Airliner* a = aviation_.airliner(ride_jet_);
    if (!a) {
      ride_jet_ = -1;
      return;
    }
    const std::string kt = std::to_string(static_cast<int>(a->v * 1.944));
    const std::string ft = std::to_string(static_cast<int>(std::max(0.0, (a->pos.z - 2.4 - ap.rwy_a.z) * 3.281) / 100.0) * 100);
    using P = Airliner::Phase;
    switch (a->phase) {
      case P::AtStand:
        prompt_ = jet_flown_ ? tr("jet.alight") : tr("jet.boarding");
        if (e && (jet_flown_ || a->timer > 20.0)) {
          aviation_.setAboard(a->id, false);
          ride_jet_ = -1;
          player_.pos = landside;
          player_.snapToGround(world_);
          player_.vel_z = 0;
          toast(tr(jet_flown_ ? "jet.alighted" : "jet.left_before"));
        }
        break;
      case P::Pushback:
      case P::TaxiOut: prompt_ = tr("jet.taxi_out"); break;
      case P::Takeoff: prompt_ = i18n_.f("jet.takeoff", {{"kt", kt}}); break;
      case P::Climb: prompt_ = i18n_.f("jet.climb", {{"ft", ft}, {"kt", kt}}); break;
      case P::Offmap: prompt_ = tr("jet.mainland"); break;
      case P::Approach: prompt_ = i18n_.f("jet.approach", {{"ft", ft}, {"kt", kt}}); break;
      case P::Landing: prompt_ = i18n_.f("jet.landing", {{"kt", kt}}); break;
      case P::TaxiIn: prompt_ = tr("jet.taxi_in"); break;
    }
    return;
  }
  if (flying_) return;
  if (!prompt_.empty() || player_.fly) return;
  if (std::hypot(player_.pos.x - landside.x, player_.pos.y - landside.y) < 50.0) {
    if (const Airliner* a = aviation_.boardable()) {
      const int64_t fare = 12800;
      prompt_ = i18n_.f("jet.board", {{"fare", std::to_string(fare)}});
      if (e) {
        if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(),
                                          tr("jet.airline")) != rj::econ::TxResult::Ok) {
          toast(tr("rail.no_money"));
          return;
        }
        ride_jet_ = a->id;
        jet_flown_ = false;
        aviation_.setAboard(a->id, true);
        jet_look_yaw_ = -1.25f;
        jet_look_pitch_ = -0.12f;
        toast(i18n_.f("jet.boarded", {{"fare", std::to_string(fare)}}));
      }
    } else {
      prompt_ = tr("jet.wait");
    }
    return;
  }
  const LightPlane& pl = aviation_.plane();
  if (!pl.crashed() && pl.onGround() && std::hypot(player_.pos.x - pl.pos().x, player_.pos.y - pl.pos().y) < 6.5 &&
      std::fabs(player_.pos.z - pl.pos().z) < 3.0) {
    prompt_ = tr("fly.enter");
    if (e) {
      flying_ = true;
      fly_cockpit_ = true;
      fly_look_yaw_ = fly_look_pitch_ = 0.0f;
      plane_in_ = PlaneControls{};
      plane_in_.brake = true;
      toast(tr("fly.entered"));
    }
  }
}

void App::updateFerryActions() {
  if (!ferries_.loaded() || !inside_id_.empty() || driving_.active() || ride_train_ >= 0 || ride_jet_ >= 0 || flying_) return;
  const bool e = IsKeyPressed(KEY_E) || autoKeyE();
  if (ride_ferry_ >= 0) {
    const Ferry* f = ferries_.ship(ride_ferry_);
    if (!f) {
      ride_ferry_ = -1;
      return;
    }
    const int at = ferries_.currentPier(*f);
    const std::string kn = std::to_string(static_cast<int>(std::lround(std::fabs(f->v) * 1.944)));
    if (f->phase == Ferry::Phase::Docked && at >= 0 && f->timer > 3.0) {
      prompt_ = i18n_.f("ferry.alight", {{"pier", pierName(at)}});
      if (e) {
        player_.pos = ferries_.gangwayPier(*f);
        player_.snapToGround(world_);
        player_.vel_z = 0;
        player_.fly = false;
        ride_ferry_ = -1;
        toast(i18n_.f("ferry.alighted", {{"pier", pierName(at)}}));
      }
    } else if (f->phase == Ferry::Phase::Offmap) {
      prompt_ = tr("ferry.mainland");
    } else {
      const int dest = ferries_.destinationPier(*f);
      prompt_ = dest >= 0 ? i18n_.f("ferry.under_way", {{"pier", pierName(dest)}, {"kn", kn}}) : i18n_.f("ferry.to_mainland", {{"kn", kn}});
    }
    return;
  }
  if (!prompt_.empty() || player_.fly) return;
  const int pi = ferries_.pierNear(player_.pos, 20.0);
  if (pi < 0 || std::fabs(player_.pos.z - (ferries_.piers()[static_cast<size_t>(pi)].pos.z + 2.4)) > 2.5) return;
  const Ferry* f = ferries_.dockedAt(pi);
  if (!f) {
    prompt_ = i18n_.f("ferry.wait", {{"pier", pierName(pi)}});
    return;
  }
  const int dest = ferries_.destinationPier(*f);
  const int64_t fare = f->cls == 0 ? 480 : 2400;
  prompt_ = i18n_.f("ferry.board", {{"dest", dest >= 0 ? pierName(dest) : tr("ferry.dest.mainland")}, {"fare", std::to_string(fare)}});
  if (e) {
    if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(),
                                      tr("ferry.name")) != rj::econ::TxResult::Ok) {
      toast(tr("rail.no_money"));
      return;
    }
    const ShipClass& C = Ferries::shipClass(f->cls);
    double sx, sy;
    ferries_.toShip(*f, player_.pos, sx, sy);
    ferry_x_ = sx > 0 ? C.deck_x - 0.6 : -(C.deck_x - 0.6);
    ferry_y_ = std::clamp(sy, C.deck_y0 + 2.0, C.deck_y1 - 2.0);
    ferry_look_yaw_ = sx > 0 ? 0.35f : -0.35f;  // looking forward along the side deck, a little out to sea
    ride_ferry_ = f->id;
    toast(i18n_.f("ferry.boarded", {{"fare", std::to_string(fare)}}));
  }
}

void App::updateDriveActions() {
  if (!inside_id_.empty() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_) return;
  const bool e = IsKeyPressed(KEY_E);
  if (driving_.active()) {
    const Vehicle& c = driving_.car();
    prompt_ = std::fabs(c.v) < 2.0 ? tr("drive.stopped") : std::string();
    if (e && std::fabs(c.v) < 2.0) {
      player_.pos = driving_.exitPosition();
      player_.snapToGround(world_);
      player_.vel_z = 0;
      player_.fly = false;
      player_.yaw = c.yaw;
      driving_.leave();
      toast(tr("drive.left"));
    }
    return;
  }
  if (!prompt_.empty() || player_.fly) return;  // another action has the E key
  auto enter = [&]() {
    drive_look_yaw_ = 0.0f;
    drive_look_pitch_ = -0.08f;
    toast(tr("drive.entered"));
  };
  if (driving_.hasCar()) {
    const Vehicle& c = driving_.car();
    const double d = std::hypot(c.pos.x - player_.pos.x, c.pos.y - player_.pos.y);
    if (d > 400.0) {
      driving_.drop();  // left far behind: the car is towed away
    } else if (d < 3.6 && std::fabs(c.pos.z - player_.pos.z) < 2.0) {
      prompt_ = tr("drive.enter_own");
      if (e) {
        driving_.enterParked();
        enter();
      }
      return;
    }
  }
  // A car stopped (or crawling) next to the player: signals, queues, junctions.
  const Vehicle* best = nullptr;
  double bd = 3.8;
  for (const auto& v : traffic_.vehicles()) {
    if (v.v > 4.0 || std::fabs(v.pos.z - player_.pos.z) > 2.0) continue;
    const double d = std::hypot(v.pos.x - player_.pos.x, v.pos.y - player_.pos.y) - Traffic::lengthOf(v.type) * 0.3;
    if (d < bd) {
      bd = d;
      best = &v;
    }
  }
  if (!best) return;
  prompt_ = tr(std::string("drive.enter.") + std::to_string(static_cast<int>(best->type)));
  if (e) {
    Vehicle v;
    if (traffic_.take(best->id, v)) {
      driving_.enter(v);
      enter();
    }
  }
}

void App::updateTransportActions() {
  if (!trains_.loaded() || !inside_id_.empty() || driving_.active() || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_ || drive_train_ >= 0) return;
  const bool e = IsKeyPressed(KEY_E) || autoKeyE(), x = IsKeyPressed(KEY_X);
  const auto& stations = trains_.stations();
  if (ride_train_ >= 0) {
    const Train* t = trains_.train(ride_train_);
    if (!t) {
      ride_train_ = -1;
      return;
    }
    if (t->at_station >= 0) {
      const Station& st = stations[static_cast<size_t>(t->at_station)];
      prompt_ = i18n_.f("rail.alight", {{"station", st.name}});
      if (e) {
        rj::geo::Vec3d p;
        float yaw, pitch;
        trains_.carPose(*t, ride_car_, p, yaw, pitch);
        const auto kind = trains_.lines()[static_cast<size_t>(t->line)].kind;
        const double side = Trains::platformOffset(kind) - Trains::trackOffset(kind);  // platform beyond the left side
        player_.pos = {p.x - std::cos(yaw) * side, p.y + std::sin(yaw) * side, st.pos.z + 0.05};
        player_.vel_z = 0;
        player_.fly = false;
        player_.yaw = yaw;
        ride_train_ = -1;
        toast(i18n_.f("rail.alighted", {{"station", st.name}}));
      }
    } else if (t->next_stop >= 0) {
      prompt_ = i18n_.f("rail.next", {{"line", lineName(t->line)}, {"station", stations[static_cast<size_t>(t->next_stop)].name},
                                      {"kmh", std::to_string(static_cast<int>(t->v * 3.6))}});
    } else {
      prompt_ = i18n_.f("rail.running", {{"line", lineName(t->line)}, {"kmh", std::to_string(static_cast<int>(t->v * 3.6))}});
    }
    return;
  }
  const int si = trains_.stationNear(player_.pos, 190.0);
  if (si < 0) return;
  const Station& st = stations[static_cast<size_t>(si)];
  const auto kind = trains_.lines()[static_cast<size_t>(st.line)].kind;
  const double hd = st.heading * DEG2RAD;
  const double sx = std::cos(hd), sy = -std::sin(hd);  // platform side axis (right of the heading)
  const bool on_platform = std::fabs(player_.pos.z - st.pos.z) < 2.2;
  if (on_platform) {
    // the stopped train on this platform's side
    const double side = (player_.pos.x - st.pos.x) * sx + (player_.pos.y - st.pos.y) * sy;
    int tid = -1;
    for (const auto& t : trains_.trains()) {
      if (t.at_station != si || t.dwell < 3.0) continue;
      rj::geo::Vec3d p;
      float yaw, pitch;
      trains_.carPose(t, t.cars / 2, p, yaw, pitch);
      const double ts = (p.x - st.pos.x) * sx + (p.y - st.pos.y) * sy;
      if (ts * side > 0) tid = t.id;
    }
    if (tid >= 0) {
      const Train* t = trains_.train(tid);
      std::string dest = trains_.destination(*t);
      if (dest == "loop+") dest = tr("rail.dest.outer");
      else if (dest == "loop-") dest = tr("rail.dest.inner");
      else if (dest == "mainland") dest = tr("rail.dest.mainland");
      prompt_ = i18n_.f("rail.board", {{"line", lineName(t->line)}, {"dest", dest}});
      if (e) {
        const int64_t fare = kind == LineKind::Shinkansen ? 2980 : 170;
        if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare,
                                          clock_.unixUtc(), lineName(t->line)) != rj::econ::TxResult::Ok) {
          toast(tr("rail.no_money"));
        } else {
          // the car nearest to the player
          double best = 1e30;
          for (int k = 0; k < t->cars; ++k) {
            rj::geo::Vec3d p;
            float yaw, pitch;
            trains_.carPose(*t, k, p, yaw, pitch);
            const double d = std::hypot(p.x - player_.pos.x, p.y - player_.pos.y);
            if (d < best) {
              best = d;
              ride_car_ = k;
            }
          }
          ride_train_ = tid;
          ride_look_yaw_ = kind == LineKind::Shinkansen ? -1.0f : 1.5708f;  // out of the window / across the car
          ride_look_pitch_ = -0.05f;
          toast(i18n_.f("rail.boarded", {{"line", lineName(t->line)}, {"fare", std::to_string(fare)}}));
        }
      }
    } else {
      prompt_ = tr("rail.wait");
    }
    if (x) {  // out through the gates, onto the street in front of the station
      player_.pos = {st.pos.x - sx * 30.0, st.pos.y - sy * 30.0, st.pos.z};
      player_.snapToGround(world_);
      toast(i18n_.f("rail.exited", {{"station", st.name}}));
    }
    return;
  }
  if (std::hypot(player_.pos.x - st.pos.x, player_.pos.y - st.pos.y) < 55.0) {
    prompt_ = i18n_.f("rail.enter", {{"station", st.name}});
    if (e) {
      const double side = ((player_.pos.x - st.pos.x) * sx + (player_.pos.y - st.pos.y) * sy) >= 0 ? 1.0 : -1.0;
      const double off = Trains::platformOffset(kind);
      player_.pos = {st.pos.x + sx * off * side, st.pos.y + sy * off * side, st.pos.z + 0.05};
      player_.vel_z = 0;
      player_.fly = false;
      toast(i18n_.f("rail.entered", {{"station", st.name}}));
    }
  }
}

void App::runSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    TraceLog(ok ? LOG_INFO : LOG_ERROR, "RJ: SELFTEST %s %s", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  const bool fic = world_.meta().fictional;
  if (fic) check(world_.residentCount() > 0, "world cells streamed around the start (island)");
  else check(world_.residentCount() == world_.knownCount(), "all world cells loaded");
  if (fic) check(world_.buildingCount() > 500, "buildings present (generated island)");
  else check(world_.buildingCount() > 9000, "buildings present (PLATEAU)");
  check(town_.size() > 0, "residents loaded");
  check(peds_.navReady(), "pedestrian navigation grid built");
  newGame();
  check(in_session_, "new game started");
  check(world_.terrainHeight(player_.pos.x, player_.pos.y).has_value(), "player spawned on terrain");
  const auto g0 = world_.toGeodetic(player_.pos);
  check(std::fabs(g0.lat_deg - world_.meta().spawn_lat) < 1e-6 && std::fabs(g0.lon_deg - world_.meta().spawn_lon) < 1e-6,
        "spawn at configured coordinates");
  const auto spawn = player_.pos;
  check(saveSlot(3), "save to slot 3");
  const auto s = readSave(3);
  check(s.has_value() && std::fabs(s->lat - g0.lat_deg) < 1e-7, "save file readable with same position");
  player_.pos.x += 250.0;
  player_.pos.y -= 120.0;
  check(loadSlot(3), "load slot 3");
  check(std::hypot(player_.pos.x - spawn.x, player_.pos.y - spawn.y) < 0.05, "position restored after load");
  check(ledger_ && ledger_->balance(player_account_) == kStartMoney, "money restored after load");
  if (fic) {
    // island: transport systems and the player's activities
    if (!traffic_placed_ && traffic_.loaded()) placeRoads();
    check(trains_.loaded() && trains_.lines().size() == 3 && trains_.stations().size() >= 8, "rail lines and stations loaded");
    check(!trains_.trains().empty(), "trains in service");
    check(ferries_.loaded() && ferries_.ships().size() == 2, "ferries loaded");
    check(aviation_.loaded() && aviation_.airport().stands.size() == 6 && aviation_.airliners().size() == 2, "airport, stands and flights loaded");
    {
      Driving d;
      Vehicle v;
      v.type = VehicleType::Sedan;
      double hd = 0;
      v.pos = player_.pos;
      traffic_.nearestLane(player_.pos, 0.0, v.pos, hd);
      v.yaw = static_cast<float>(hd);
      d.enter(v);
      DriveInput in;
      in.throttle = 1;
      for (int k = 0; k < 90; ++k) d.update(1.0 / 60.0, world_, traffic_, in);
      check(d.car().v > 3.0 && d.rpm() > 1000.0f, "car accelerates (vehicle dynamics)");
    }
    {
      Jobs j;
      check(j.startDelivery(world_, traffic_, player_.pos), "delivery job found nearby");
    }
    check(saveSlot(3) && loadSlot(3), "save / load on the island");
  }
  // Verified interior: enter through a real entrance, stand on a real floor, walls block.
  if (!fic) check(!world_.meta().interiors.empty(), "interior listed in slice");
  if (!world_.meta().interiors.empty()) {
    const auto& im = world_.meta().interiors.front();
    const bool loaded = world_.forceLoadInterior(im.id);
    check(loaded, "interior package loads");
    if (const Interior* in = world_.interior(im.id); in && !in->entrances().empty()) {
      const auto& e = in->entrances().front();
      check(in->floorBelow(e.inside.x, e.inside.y, e.inside.z + 0.5, 2.0).has_value(), "entrance lands on an interior floor");
      check(e.inside.z < e.street.z - 0.2, "interior entrance is below street level");
      player_.pos = {e.inside.x, e.inside.y, e.inside.z + 0.05};
      inside_id_ = im.id;
      const auto before = player_.pos;
      for (int k = 0; k < 120; ++k) player_.update(1.0f / 60.0f, world_, settings_, false, in);
      check(std::hypot(player_.pos.x - before.x, player_.pos.y - before.y) < 0.6 && player_.grounded, "player stands inside");
      check(saveSlot(3) && readSave(3) && readSave(3)->interior == im.id, "interior state saved");
      inside_id_.clear();
      player_.pos = spawn;
    }
  }
  const std::string lang0 = settings_.language;
  setLanguage("en");
  const std::string en = tr("menu.new_game");
  setLanguage("ja");
  const std::string ja = tr("menu.new_game");
  check(en == "New Game" && ja != en && ja != "menu.new_game", "Japanese and English strings");
  Settings reread;
  reread.load(dataDir() / "config" / "default.ini", userDir() / "settings.ini");
  check(reread.language == "ja", "settings.ini written and re-read");
  setLanguage(lang0);
  const auto hist = town_.histogram(clock_.jst().date, clock_.jst().minuteOfDay());
  int total = 0;
  for (int h : hist) total += h;
  check(total == static_cast<int>(town_.size()), "every resident has an activity now");
  std::error_code ec;
  std::filesystem::remove(savePath(3), ec);
  TraceLog(fails ? LOG_ERROR : LOG_INFO, "RJ: SELFTEST %s (%d failed)", fails ? "FAILED" : "OK", fails);
  exit_code_ = fails ? 3 : 0;
  quit_ = true;
}

Camera3D App::titleCamera() const {
  const float a = title_t_ * 0.035f + 0.6f;
  const double r = 420.0;
  const rj::geo::Vec3d target{0, 0, 25};
  const rj::geo::Vec3d pos{std::sin(a) * r, std::cos(a) * r, 210.0};
  Camera3D c{};
  c.position = enuToRl(pos);
  c.target = enuToRl(target);
  c.up = {0, 1, 0};
  c.fovy = 55.0f;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

std::vector<PointLight> App::collectLights(const Camera3D& cam) const {
  std::vector<PointLight> out;
  if (lighting_.night < 0.03f || lighting_.indoor > 0.9f) return out;
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  struct Cand {
    double d2;
    PointLight l;
  };
  std::vector<Cand> cand;
  const float k = lighting_.night;
  for (const auto& [code, cell] : world_.cells())
    for (const auto& l : cell->lights) {  // real street-light heads (PLATEAU frn 4200)
      const double dx = l.pos.x - c.x, dy = l.pos.y - c.y;
      const double d2 = dx * dx + dy * dy;
      if (d2 > 180.0 * 180.0) continue;
      cand.push_back({d2, {enuToRl(l.pos), l.range * 1.4f, Vector3Scale(Vector3{1.0f, 0.90f, 0.76f}, 26.0f * k)}});
    }
  for (const auto& l : markings_.lights()) {  // estimated road lights / street lamps
    const double dx = l.pos.x - c.x, dy = l.pos.y - c.y;
    const double d2 = dx * dx + dy * dy;
    if (d2 > 180.0 * 180.0) continue;
    cand.push_back({d2, {enuToRl(l.pos), l.range * 1.4f, Vector3Scale(Vector3{1.0f, 0.93f, 0.84f}, 26.0f * k * l.intensity)}});
  }
  std::vector<rj::geo::Vec3d> shops;
  facades_.collectLights(c, 90.0, shops);  // lit shop fronts spill onto the sidewalk
  for (const auto& p : shops) {
    const double dx = p.x - c.x, dy = p.y - c.y;
    cand.push_back({dx * dx + dy * dy + 400.0, {enuToRl(p), 9.0f, Vector3Scale(Vector3{1.0f, 0.93f, 0.82f}, 7.0f * std::max(k, 0.25f) * lighting_.occupancy.z)}});
  }
  // headlights: the player's car throws light on the road ahead; nearby traffic too
  auto beam = [&](const Vehicle& v, double ahead, float range, float power, double prio) {
    const rj::geo::Vec3d p{v.pos.x + std::sin(v.yaw) * ahead, v.pos.y + std::cos(v.yaw) * ahead, v.pos.z + 0.9};
    cand.push_back({prio, {enuToRl(p), range, Vector3Scale(Vector3{1.0f, 0.95f, 0.86f}, power * k)}});
  };
  if (driving_.hasCar() && driving_.active()) {
    beam(driving_.car(), 7.0, 15.0f, 20.0f, 0.0);
    beam(driving_.car(), 17.0, 22.0f, 14.0f, 1.0);
  }
  {
    std::vector<std::pair<double, const Vehicle*>> near;
    for (const auto& v : traffic_.vehicles()) {
      const double dx = v.pos.x - c.x, dy = v.pos.y - c.y, d2 = dx * dx + dy * dy;
      if (d2 < 140.0 * 140.0) near.push_back({d2, &v});
    }
    std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < near.size() && i < 6; ++i) beam(*near[i].second, 7.0, 13.0f, 9.0f, near[i].first + 900.0);
  }
  std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
  for (size_t i = 0; i < cand.size() && out.size() < Renderer::kMaxLights; ++i) out.push_back(cand[i].l);
  return out;
}

void App::drawWorldView(const Camera3D& cam) {
  const Interior* in = (screen_ != Screen::Title) ? insideInterior() : nullptr;
  const bool deep = in && underground_ > 0.98f;  // nothing of the outside is visible any more
  RenderOptions ro;
  static const bool no_shadows = std::getenv("RJ_NO_SHADOWS") != nullptr;  // debug isolation
  ro.shadows = settings_.shadows && underground_ < 0.5f && !no_shadows;
  ro.post = settings_.post_fx;
  ro.ssao = settings_.post_fx;
  ro.bloom = settings_.post_fx;
  if (ro.shadows) {
    std::vector<Renderer::Caster> casters;
    facades_.forEachMesh([&](const Mesh& m, int lod) {
      if (lod == 0) casters.push_back({&m, MatrixIdentity()});
    });
    renderer_.vehicleCasters(traffic_, cam, casters, driving_.hasCar() ? &driving_.car() : nullptr);
    renderer_.renderShadowMaps(cam, world_, lighting_, casters);
  }
  // Driver's seat: rear view for the mirrors (every other frame)
  const bool cockpit_view = driving_.active() && drive_first_person_ && screen_ != Screen::Title && !deep;
  if (!cockpit_view) {
    renderer_.invalidateMirror();
  } else if (frame_ % 2 == 0 || shot_frames_ >= 0) {
    Renderer::setClipPlanes(0.2f, 900.0f);
    const Camera3D rear = driving_.rearCamera();
    renderer_.setLights(collectLights(rear));
    renderer_.renderMirror(rear, lighting_, render_time_, [&]() {
      renderer_.drawWorld(rear, world_, settings_.photo_textures, !world_.meta().fictional);
      renderer_.drawMarkings(markings_);
      if (world_.meta().fictional) {
        const auto g = world_.toGeodetic(rlToEnu(rear.position));
        renderer_.drawOcean(rear, static_cast<float>(world_.toLocal({g.lat_deg, g.lon_deg, 0.0}).z));
      }
      renderer_.drawVehicles(traffic_, rear, lighting_);
      renderer_.drawTrains(trains_, rear, -1, 0);
      renderer_.drawShips(ferries_, rear, lighting_);
      renderer_.drawAircraft(aviation_, rear, lighting_, -1, nullptr);
      renderer_.drawPedestrians(peds_, lighting_.rain);
    });
  }
  // Indoors walls can be 0.3 m from the eye: pull the near plane in so it never cuts through them.
  Renderer::setClipPlanes(in ? 0.08f : (cockpit_view ? 0.05f : 0.2f), static_cast<float>(settings_.view_distance_m) * 1.6f + 500.0f);
  renderer_.beginScene(ro, lighting_, cam, render_time_);
  ClearBackground(deep ? Color{58, 58, 60, 255} : BLACK);
  const float aspect = static_cast<float>(GetScreenWidth()) / static_cast<float>(std::max(1, GetScreenHeight()));
  if (!deep) renderer_.drawSky(cam, lighting_, aspect);
  renderer_.setLights(collectLights(cam));
  BeginMode3D(cam);
  renderer_.drawWorld(cam, world_, settings_.photo_textures, !in && !world_.meta().fictional);
  if (!deep) renderer_.drawMarkings(markings_);
  if (world_.meta().fictional && !deep) {
    // Sea level (height 0) under the camera, in the floating-origin frame.
    const auto g = world_.toGeodetic(rlToEnu(cam.position));
    renderer_.drawOcean(cam, static_cast<float>(world_.toLocal({g.lat_deg, g.lon_deg, 0.0}).z));
  }
  static const bool no_facades = std::getenv("RJ_NO_FACADES") != nullptr;      // debug isolation
  static const bool no_int_out = std::getenv("RJ_NO_INTERIOR_OUT") != nullptr;
  if (!deep) {
    if (!no_facades) renderer_.drawFacades(facades_);
    renderer_.drawSignals(signals_, cam);
    Renderer::CockpitView cv;
    const bool cockpit = driving_.active() && drive_first_person_ && screen_ != Screen::Title;
    if (cockpit) {
      cv.kmh = driving_.speedKmh();
      cv.rpm = driving_.rpm();
      cv.steer = driving_.steerAngle();
    }
    renderer_.drawVehicles(traffic_, cam, lighting_, driving_.hasCar() ? &driving_.car() : nullptr, cockpit ? &cv : nullptr);
    renderer_.drawTrains(trains_, cam, ride_train_, ride_car_);
    renderer_.drawShips(ferries_, cam, lighting_);
    Renderer::FlightView fv;
    if (flying_) {
      const LightPlane& pl = aviation_.plane();
      fv.cockpit = fly_cockpit_;
      fv.kt = static_cast<float>(pl.airspeed() * 1.944);
      const auto g = world_.toGeodetic(pl.pos());
      fv.alt_ft = static_cast<float>((pl.pos().z - world_.toLocal({g.lat_deg, g.lon_deg, 0.0}).z) * 3.281);
      fv.vs_fpm = static_cast<float>(pl.verticalSpeed() * 196.85);
      fv.heading = static_cast<float>(pl.heading());
      fv.pitch = static_cast<float>(pl.pitchDeg());
      fv.roll = static_cast<float>(pl.rollDeg());
      fv.elevator = pl.controls().elevator;
      fv.aileron = pl.controls().aileron;
    }
    renderer_.drawAircraft(aviation_, cam, lighting_, ride_jet_, flying_ ? &fv : nullptr);
  }
  if (in) {
    renderer_.drawInterior(*in);
  } else if (in_session_ && screen_ != Screen::Title && !no_int_out) {
    // From the street the stairs are visible through the real openings cut into the pavement.
    for (const auto& [id, interior] : world_.interiors()) renderer_.drawInterior(*interior);
  }
  if (in_session_ && screen_ != Screen::Title && !deep) renderer_.drawPedestrians(peds_, lighting_.rain);
  if (in_session_ && screen_ != Screen::Title && !deep && !photo_request_) drawWorldMarkers(cam);
  if (in_session_ && player_.camera_mode == 1 && screen_ != Screen::Title && ride_train_ < 0 && !driving_.active() && ride_jet_ < 0 && !flying_)
    renderer_.drawPlayerBody(enuToRl(player_.pos), player_.yaw);
  renderer_.drawRain(cam, lighting_, render_time_);
  EndMode3D();
  renderer_.endScene(cam, lighting_, render_time_);
}

void App::draw() {
  switch (screen_) {
    case Screen::Boot:
      drawBootScreen();
      if (frame_ >= 1) {
        if (boot()) screen_ = Screen::Loading;
        else screen_ = Screen::Fatal;
      }
      return;
    case Screen::Fatal:
      drawFatal();
      return;
    case Screen::Loading:
      drawLoading();
      return;
    default:
      break;
  }
  const bool game_view = in_session_ && screen_ != Screen::Title && screen_ != Screen::Credits &&
                         !(screen_ == Screen::Settings && settings_return_ == Screen::Title) &&
                         !(screen_ == Screen::Slots && slots_return_ == Screen::Title);
  float third_dist = 4.5f;
  if (const Interior* in = insideInterior(); in && player_.camera_mode == 1) {
    // Keep the chase camera on this side of the nearest wall.
    const rj::geo::Vec3d f = player_.forwardEnu();
    rj::geo::Vec3d back{-f.x * 4.5, -f.y * 4.5, -f.z * 4.5 + 0.6};
    const double len = std::sqrt(back.x * back.x + back.y * back.y + back.z * back.z);
    back = {back.x / len, back.y / len, back.z / len};
    if (auto hit = in->raycast(player_.eyeEnu(), back, len))
      third_dist = static_cast<float>(std::max(0.3, (*hit - 0.25) / len * 4.5));
  }
  Camera3D view_cam;
  if (!game_view) view_cam = titleCamera();
  else if (ride_train_ >= 0) view_cam = drive_train_ >= 0 ? cabCamera() : rideCamera();
  else if (ride_jet_ >= 0) view_cam = jetCamera();
  else if (flying_) view_cam = aviation_.plane().camera(settings_.fov, fly_cockpit_, fly_look_yaw_, fly_look_pitch_);
  else if (driving_.active()) view_cam = driving_.camera(settings_.fov, drive_first_person_, drive_look_yaw_, drive_look_pitch_);
  else view_cam = player_.camera(photo_mode_ && photo_fov_ > 0 ? photo_fov_ : settings_.fov, third_dist);
  drawWorldView(view_cam);
  if (photo_request_) {  // the frame as seen, before the HUD is drawn
    photo_request_ = false;
    takePhoto();
  }

  switch (screen_) {
    case Screen::Title: drawTitle(); break;
    case Screen::Settings: drawSettings(); break;
    case Screen::Slots: drawSlots(); break;
    case Screen::Credits: drawCredits(); break;
    case Screen::Game: drawHud(); break;
    case Screen::Pause:
      drawHud();
      drawPause();
      break;
    case Screen::Phone:
      drawHud();
      drawPhone();
      break;
    default: break;
  }
  if (settings_.show_fps) ui_.textRight(i18n_.f("hud.fps", {{"n", std::to_string(GetFPS())}}), ui_.vw() - 20, 1040, 24, theme::kMuted);
}

// ---------------------------------------------------------------------------
void App::drawBootScreen() {
  ClearBackground(Color{8, 9, 12, 255});
  const int w = GetScreenWidth(), h = GetScreenHeight();
  DrawCircle(w / 2, h / 2 - 40, static_cast<float>(h) * 0.06f, Color{214, 0, 40, 255});
  const char* t = "PROJECT: REAL JAPAN";
  const int fs = h / 20;
  DrawText(t, (w - MeasureText(t, fs)) / 2, h / 2 + h / 14, fs, RAYWHITE);
  DrawText("Loading...", (w - MeasureText("Loading...", fs / 2)) / 2, h / 2 + h / 14 + fs + 10, fs / 2, GRAY);
}

void App::drawFatal() {
  ClearBackground(Color{20, 8, 10, 255});
  DrawText("PROJECT: REAL JAPAN - startup error", 40, 40, 30, RAYWHITE);
  if (ui_.hasFont()) {  // Japanese / English message (the default font has ASCII only)
    ui_.textWrapped(fatal_, 40, 100, ui_.vw() - 80, 26, Color{255, 180, 180, 255});
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) quit_ = true;
    return;
  }
  DrawText(fatal_.c_str(), 40, 100, 20, Color{255, 180, 180, 255});
  DrawText("Please re-extract the game folder (data/ must be next to RealJapan.exe).", 40, 140, 20, GRAY);
  if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) quit_ = true;
}

void App::drawLoading() {
  ClearBackground(Color{8, 9, 12, 255});
  const float cx = ui_.vw() / 2;
  DrawCircle(static_cast<int>(cx * ui_.scale()), static_cast<int>(420 * ui_.scale()), 60 * ui_.scale(), theme::kAccent);
  ui_.textCentered(tr("title.name"), cx, 520, 64, theme::kText);
  const std::string msg = i18n_.f("loading.cells", {{"n", std::to_string(world_.residentCount())},
                                                    {"total", std::to_string(world_.knownCount())}});
  ui_.textCentered(msg, cx, 620, 32, theme::kMuted);
  const float pw = 700, px = cx - pw / 2;
  DrawRectangleRec(ui_.px({px, 680, pw, 10}), Color{40, 40, 48, 255});
  const float f = world_.knownCount() ? static_cast<float>(world_.residentCount()) / static_cast<float>(world_.knownCount()) : 0;
  DrawRectangleRec(ui_.px({px, 680, pw * f, 10}), theme::kAccent);
  ui_.textCentered(tr("credits.plateau"), cx, 980, 22, theme::kMuted);
  ui_.textCentered(tr("credits.gsi"), cx, 1012, 22, theme::kMuted);
}

void App::drawTitle() {
  const float vw = ui_.vw();
  DrawRectangleGradientH(0, 0, static_cast<int>(900 * ui_.scale()), GetScreenHeight(), Color{0, 0, 0, 200}, Color{0, 0, 0, 0});
  DrawCircle(static_cast<int>(130 * ui_.scale()), static_cast<int>(150 * ui_.scale()), 34 * ui_.scale(), theme::kAccent);
  ui_.text(tr("title.name"), 190, 112, 64, theme::kText);
  ui_.text(tr("title.subtitle"), 192, 190, 30, theme::kMuted);
  const std::string place = i18n_.code() == "ja" ? world_.meta().name_ja : world_.meta().name_en;
  ui_.text(place + "  ·  " + dateTimeString(), 192, 232, 26, theme::kMuted);

  float y = 330;
  const float x = 110, w = 520, h = 70, gap = 18;
  const auto latest = latestSlot();
  if (ui_.button({x, y, w, h}, tr("menu.continue"), latest.has_value())) loadSlot(*latest);
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.new_game"))) newGame();
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.load"))) {
    slots_saving_ = false;
    slots_return_ = Screen::Title;
    screen_ = Screen::Slots;
  }
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.settings"))) {
    settings_return_ = Screen::Title;
    screen_ = Screen::Settings;
  }
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.credits"))) screen_ = Screen::Credits;
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.quit"))) quit_ = true;

  ui_.text(tr("title.build"), 110, 1000, 22, theme::kMuted);
  ui_.textRight("v0.2.0  ·  " + std::to_string(world_.buildingCount()) + " buildings (PLATEAU)", vw - 30, 1040, 20, theme::kMuted);
}

void App::drawSettings() {
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 150});
  const float vw = ui_.vw();
  const float w = 1100, x = (vw - w) / 2;
  ui_.panel({x - 30, 20, w + 60, 1045});
  ui_.text(tr("settings.title"), x, 38, 44, theme::kText);
  float y = 105;
  const float rh = 52, gap = 6;
  auto onoff = [&](bool v) { return tr(v ? "settings.on" : "settings.off"); };
  bool changed = false;

  if (int d = ui_.stepper({x, y, w, rh}, tr("settings.language"), tr("lang.name")); d) {
    setLanguage(settings_.language == "ja" ? "en" : "ja");
  }
  y += rh + gap;
  {
    const auto& R = Settings::resolutions();
    int idx = 0;
    for (size_t i = 0; i < R.size(); ++i)
      if (R[i].first == settings_.width && R[i].second == settings_.height) idx = static_cast<int>(i);
    const std::string v = std::to_string(settings_.width) + " × " + std::to_string(settings_.height);
    if (int d = ui_.stepper({x, y, w, rh}, tr("settings.resolution"), v); d) {
      idx = (idx + d + static_cast<int>(R.size())) % static_cast<int>(R.size());
      settings_.width = R[static_cast<size_t>(idx)].first;
      settings_.height = R[static_cast<size_t>(idx)].second;
      changed = true;
      if (!settings_.fullscreen) SetWindowSize(settings_.width, settings_.height);
    }
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.fullscreen"), onoff(settings_.fullscreen))) {
    settings_.fullscreen = !settings_.fullscreen;
    changed = true;
    applyWindowMode();
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.vsync"), onoff(settings_.vsync))) {
    settings_.vsync = !settings_.vsync;
    changed = true;
    if (settings_.vsync) SetWindowState(FLAG_VSYNC_HINT);
    else ClearWindowState(FLAG_VSYNC_HINT);
  }
  y += rh + gap;
  if (int d = ui_.stepper({x, y, w, rh}, tr("settings.fov"), std::to_string(static_cast<int>(settings_.fov)) + "°"); d) {
    settings_.fov = std::clamp(settings_.fov + 5.0f * d, 50.0f, 100.0f);
    changed = true;
  }
  y += rh + gap;
  if (int d = ui_.stepper({x, y, w, rh}, tr("settings.mouse"), fixed(settings_.mouse_sensitivity, 1)); d) {
    settings_.mouse_sensitivity = std::clamp(settings_.mouse_sensitivity + 0.1f * d, 0.1f, 5.0f);
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.invert_y"), onoff(settings_.invert_y))) {
    settings_.invert_y = !settings_.invert_y;
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.shadows"), onoff(settings_.shadows))) {
    settings_.shadows = !settings_.shadows;
    changed = true;
  }
  y += rh + gap;
  {
    const auto& V = Settings::viewDistances();
    auto it = std::find(V.begin(), V.end(), settings_.view_distance_m);
    int idx = it == V.end() ? 1 : static_cast<int>(it - V.begin());
    if (int d = ui_.stepper({x, y, w, rh}, tr("settings.view_distance"), std::to_string(settings_.view_distance_m) + " m"); d) {
      idx = (idx + d + static_cast<int>(V.size())) % static_cast<int>(V.size());
      settings_.view_distance_m = V[static_cast<size_t>(idx)];
      changed = true;
    }
  }
  y += rh + gap;
  {
    const auto& T = Settings::timeScales();
    auto it = std::find(T.begin(), T.end(), settings_.time_scale);
    int idx = it == T.end() ? 2 : static_cast<int>(it - T.begin());
    if (int d = ui_.stepper({x, y, w, rh}, tr("settings.time_scale"),
                            i18n_.f("settings.seconds", {{"n", std::to_string(settings_.time_scale)}}));
        d) {
      idx = (idx + d + static_cast<int>(T.size())) % static_cast<int>(T.size());
      settings_.time_scale = T[static_cast<size_t>(idx)];
      changed = true;
    }
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.real_time_start"), onoff(settings_.real_time_start))) {
    settings_.real_time_start = !settings_.real_time_start;
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.show_fps"), onoff(settings_.show_fps))) {
    settings_.show_fps = !settings_.show_fps;
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.photo_textures"), onoff(settings_.photo_textures))) {
    settings_.photo_textures = !settings_.photo_textures;
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.post_fx"), onoff(settings_.post_fx))) {
    settings_.post_fx = !settings_.post_fx;
    changed = true;
  }
  y += rh + gap;
  if (ui_.stepper({x, y, w, rh}, tr("settings.head_bob"), onoff(settings_.head_bob))) {
    settings_.head_bob = !settings_.head_bob;
    changed = true;
  }
  y += rh + gap + 6;
  if (changed) saveSettings();
  ui_.text(tr("settings.saved_note"), x, y + 18, 24, theme::kMuted);
  if (ui_.button({x + w - 300, y, 300, 64}, tr("menu.back"))) screen_ = settings_return_;
}

void App::drawSlots() {
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 150});
  const float w = 1100, x = (ui_.vw() - w) / 2;
  ui_.panel({x - 30, 150, w + 60, 720});
  ui_.text(tr(slots_saving_ ? "save.title_save" : "save.title_load"), x, 180, 44, theme::kText);
  float y = 260;
  for (int slot = 0; slot < kSaveSlots; ++slot) {
    const auto s = readSave(slot);
    const std::string name = slot == kAutosaveSlot ? tr("save.auto") : i18n_.f("save.slot", {{"n", std::to_string(slot)}});
    std::string line = tr("save.empty");
    if (s) {
      const auto gt = rj::sim::GameClock(s->game_unix).jst();
      line = i18n_.f("save.line", {{"date", s->saved_at},
                                   {"game", std::to_string(gt.date.m) + "/" + std::to_string(gt.date.d) + " " +
                                                two(gt.hour) + ":" + two(gt.minute)},
                                   {"money", withCommas(s->money)}});
    }
    const bool enabled = slots_saving_ ? (slot != kAutosaveSlot) : s.has_value();
    const Rectangle r{x, y, w, 110};
    if (ui_.button(r, "", enabled, 30)) {
      if (slots_saving_) {
        saveSlot(slot);
        screen_ = slots_return_;
      } else {
        loadSlot(slot);
      }
    }
    ui_.text(name, x + 30, y + 16, 34, enabled ? theme::kText : theme::kMuted);
    ui_.text(line, x + 30, y + 62, 26, theme::kMuted);
    y += 125;
  }
  if (ui_.button({x + w - 300, 780, 300, 64}, tr("menu.back"))) screen_ = slots_return_;
}

void App::drawCredits() {
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 175});
  const float w = 1400, x = (ui_.vw() - w) / 2;
  ui_.panel({x - 30, 40, w + 60, 1000});
  ui_.text(tr("credits.title"), x, 60, 46, theme::kText);
  float y = 135;
  ui_.text(tr("credits.data"), x, y, 30, theme::kAccent);
  y += 44;
  for (const auto& s : world_.meta().sources) {
    y += ui_.textWrapped("• " + s.attribution + "  [" + s.license + "]", x + 10, y, w - 20, 24, theme::kText) + 4;
  }
  y += ui_.textWrapped(tr("credits.notice"), x + 10, y, w - 20, 22, theme::kMuted) + 4;
  y += ui_.textWrapped(tr("credits.pending"), x + 10, y, w - 20, 22, theme::kWarn) + 18;
  ui_.text(tr("credits.software"), x, y, 30, theme::kAccent);
  y += 44;
  y += ui_.textWrapped("• " + tr("credits.raylib"), x + 10, y, w - 20, 24, theme::kText);
  y += ui_.textWrapped("• " + tr("credits.font"), x + 10, y, w - 20, 24, theme::kText) + 18;
  ui_.text(tr("credits.status"), x, y, 30, theme::kAccent);
  y += 44;
  y += ui_.textWrapped(tr("credits.done"), x + 10, y, w - 20, 24, theme::kGood) + 8;
  y += ui_.textWrapped(tr("credits.todo"), x + 10, y, w - 20, 24, theme::kWarn) + 8;
  if (ui_.button({x + w - 300, 950, 300, 64}, tr("menu.back"))) screen_ = Screen::Title;
}

// ---------------------------------------------------------------------------
void App::drawHud() {
  const float vw = ui_.vw();
  drawActivityHud();
  if (photo_mode_) return;  // viewfinder only
  const auto g = world_.toGeodetic(player_.pos);
  const auto t = jst();
  const int cx = GetScreenWidth() / 2, cy = GetScreenHeight() / 2;
  if (flying_ && screen_ == Screen::Game) {  // flight data
    const LightPlane& pl = aviation_.plane();
    const auto g = world_.toGeodetic(pl.pos());
    const double alt_ft = (pl.pos().z - world_.toLocal({g.lat_deg, g.lon_deg, 0.0}).z) * 3.281;
    const PlaneControls& pc = pl.controls();
    char buf[200];
    std::snprintf(buf, sizeof buf, "IAS %3d kt   ALT %5d ft   VS %+5d fpm   HDG %03d", static_cast<int>(pl.airspeed() * 1.944),
                  static_cast<int>(alt_ft / 10) * 10, static_cast<int>(pl.verticalSpeed() * 196.85 / 10) * 10, static_cast<int>(pl.heading()) % 360);
    char buf2[160];
    std::snprintf(buf2, sizeof buf2, "THR %3d%%   FLAPS %d°   RPM %4d", static_cast<int>(pc.throttle * 100), pc.flaps * 10, static_cast<int>(pl.rpm()));
    ui_.panel({30, 900, 760, 130}, Color{0, 0, 0, 140});
    ui_.text(buf, 50, 912, 30, theme::kText);
    ui_.text(buf2, 50, 950, 26, theme::kMuted);
    ui_.text(tr("fly.help"), 50, 992, 20, theme::kMuted);
    if (pl.stalled()) ui_.textCentered("STALL", vw / 2, 120, 48, theme::kWarn);
  }
  if (driving_.active() && screen_ == Screen::Game) {  // speedometer
    const double v = driving_.car().v;
    const std::string kmh = std::to_string(static_cast<int>(std::lround(std::fabs(v) * 3.6)));
    ui_.panel({30, 900, 400, 130}, Color{0, 0, 0, 140});
    ui_.text(kmh, 54, 912, 72, theme::kText);
    ui_.text("km/h", 64 + ui_.measure(kmh, 72), 954, 28, theme::kMuted);
    const int gear = driving_.gear();
    ui_.textRight(gear < 0 ? "R" : "D" + std::to_string(gear), 406, 912, 34, gear < 0 ? theme::kWarn : theme::kMuted);
    ui_.textRight(std::to_string(static_cast<int>(driving_.rpm() / 100.0f) * 100) + " rpm", 406, 954, 22, theme::kMuted);
    ui_.text(tr("drive.help"), 54, 992, 20, theme::kMuted);
  }
  if (!settings_.dev_overlay) {
    // Immersive view: no panels. A faint dot, context prompts and the first-minute controls hint only.
    if (screen_ == Screen::Game && player_.camera_mode == 0) DrawCircle(cx, cy, 1.6f * ui_.scale(), Color{255, 255, 255, 150});
    if (screen_ == Screen::Game && !prompt_.empty()) {
      const float w = ui_.measure(prompt_, 28) + 50;
      ui_.panel({(vw - w) / 2, 780, w, 54}, Color{0, 0, 0, 150});
      ui_.textCentered(prompt_, vw / 2, 791, 28, theme::kText);
    }
    if (session_t_ < 20.0f && screen_ == Screen::Game)
      ui_.textCentered(tr("hud.controls_short"), vw / 2, 1030, 22,
                       Color{255, 255, 255, static_cast<unsigned char>(std::min(1.0f, (20.0f - session_t_) / 3.0f) * 190)});
    return;
  }
  // Developer overlay (F3): time, place, coordinates, mesh, building data, performance.
  ui_.panel({20, 20, 640, 170});
  ui_.text(dateTimeString(), 40, 34, 40, theme::kText);
  if (auto hol = rj::sim::holidayName(t.date)) ui_.text(std::string(*hol), 470, 44, 26, theme::kWarn);
  const auto mc = rj::geo::MeshCode::fromLatLon({g.lat_deg, g.lon_deg}, 3);
  const std::string place = (i18n_.code() == "ja" ? world_.meta().name_ja : world_.meta().name_en) + "  ·  " +
                            i18n_.f("hud.cell", {{"code", mc ? mc->str() : "-"}});
  ui_.text(place, 40, 88, 24, theme::kMuted);
  ui_.text(fixed(g.lat_deg, 5) + ", " + fixed(g.lon_deg, 5) + "  ·  T.P. " + fixed(g.h_ellipsoidal_m, 1) + " m", 40, 118, 22, theme::kMuted);
  const int64_t money = ledger_ ? ledger_->balance(player_account_) : 0;
  ui_.text(i18n_.f("hud.money", {{"n", withCommas(money)}}), 40, 148, 26, theme::kText);
  ui_.textRight(tr(player_.fly ? "hud.fly" : "hud.walk"), 640, 148, 24, player_.fly ? theme::kWarn : theme::kMuted);
  if (!world_.insideData(g.lat_deg, g.lon_deg)) ui_.text(tr("hud.out_of_data"), 40, 200, 24, theme::kWarn);

  {
    const std::string perf = i18n_.f("hud.perf", {{"fps", std::to_string(GetFPS())},
                                                  {"dc", std::to_string(renderer_.drawCalls())},
                                                  {"tri", fixed(static_cast<double>(renderer_.triangles()) / 1e6, 2)},
                                                  {"exp", fixed(renderer_.exposure(), 2)}}) +
                             "  ·  " + tr(std::string("weather.") + weatherKey(weather_.kind())) +
                             (weather_.wetness() > 0.02f ? "  ·  " + i18n_.f("hud.wet", {{"n", std::to_string(static_cast<int>(weather_.wetness() * 100))}}) : "");
    ui_.text(perf, 40, 196, 22, theme::kMuted);
    const std::string life = i18n_.f("hud.life", {{"peds", std::to_string(peds_.walkers().size())},
                                                   {"vis", std::to_string(peds_.visitorCount())},
                                                   {"act", fixed(peds_.activity(), 2)},
                                                   {"cars", std::to_string(traffic_.vehicles().size())},
                                                   {"xw", std::to_string(markings_.crossings().size())},
                                                   {"est", std::to_string(markings_.estimatedCrossings())}});
    ui_.text(life, 40, 224, 22, theme::kMuted);
  }
  // Crosshair.
  if (screen_ == Screen::Game && player_.camera_mode == 0) {
    DrawCircleLines(cx, cy, 6 * ui_.scale(), Color{255, 255, 255, 180});
    DrawPixel(cx, cy, WHITE);
  }
  // Minimap bottom-right.
  if (screen_ != Screen::Phone) drawMap({vw - 360, 700, 330, 330}, 220.0, false);
  if (screen_ == Screen::Game) {
    if (insideInterior()) drawInteriorInfo();
    else if (hover_walker_) drawWalkerInfo();
    else drawBuildingInfo();
    if (!prompt_.empty()) {
      const float w = ui_.measure(prompt_, 30) + 60;
      ui_.panel({(vw - w) / 2, 760, w, 60}, Color{0, 0, 0, 190});
      ui_.textCentered(prompt_, vw / 2, 773, 30, theme::kWarn);
    }
  }
  if (session_t_ < 25.0f && screen_ == Screen::Game)
    ui_.textCentered(tr("hud.controls"), vw / 2, 1030, 24, Color{255, 255, 255, static_cast<unsigned char>(std::min(1.0f, (25.0f - session_t_) / 3.0f) * 220)});
}

void App::drawBuildingInfo() {
  if (!hover_ || !hover_->building) return;
  const BuildingInfo& b = *hover_->building;
  const float vw = ui_.vw();
  const float w = 560, x = vw - w - 30, y = 20;
  ui_.panel({x, y, w, 420});
  float yy = y + 18;
  ui_.text(tr("info.title") + "  ·  " + fixed(hover_->distance, 0) + " m", x + 22, yy, 22, theme::kMuted);
  yy += 34;
  ui_.text(b.name.empty() ? tr("info.unnamed") : b.name, x + 22, yy, 36, theme::kText);
  yy += 52;
  auto row = [&](const std::string& k, const std::string& v, Color c = theme::kText) {
    ui_.text(k, x + 22, yy, 24, theme::kMuted);
    ui_.text(v, x + 22 + std::max(170.0f, ui_.measure(k, 24) + 18), yy, 24, c);
    yy += 34;
  };
  row(tr("info.usage"), tr("usage." + std::to_string(b.usage)));
  row(tr("info.height"), b.measured_height > 0 ? fixed(b.measured_height, 1) + " m" : tr("info.unknown"));
  // PLATEAU uses 9999 for "unknown" storey counts.
  const bool storeys_known = b.storeys_above >= 0 && b.storeys_above < 9999;
  const int below = (b.storeys_below >= 0 && b.storeys_below < 9999) ? b.storeys_below : 0;
  row(tr("info.storeys"), storeys_known ? i18n_.f("info.storeys_value", {{"a", std::to_string(b.storeys_above)},
                                                                        {"b", std::to_string(below)}})
                                        : tr("info.unknown"));
  const bool fictional = b.interior_status == 3;  // INTERIOR_FICTIONAL: generated building on the invented island
  if (!fictional) row(tr("info.lod"), tr(b.lod >= 2 ? "info.lod2" : "info.lod1"));
  yy += 6;
  if (fictional) {
    ui_.text(tr("verify.badge.fictional"), x + 22, yy, 24, theme::kWarn);
    yy += 34;
    yy += ui_.textWrapped(tr("verify.interior.fictional"), x + 22, yy, w - 44, 22, theme::kMuted);
  } else {
    ui_.text(tr(b.geometry_status == 0 ? "verify.badge.verified_exterior" : "verify.badge.unverified"), x + 22, yy, 24,
             b.geometry_status == 0 ? theme::kGood : theme::kWarn);
    yy += 34;
    yy += ui_.textWrapped(tr("verify.interior.unknown"), x + 22, yy, w - 44, 22, theme::kWarn);
    yy += 4;
    yy += ui_.textWrapped(tr("info.source") + "  " + tr("info.source_short." + std::to_string(b.source_index)), x + 22, yy,
                          w - 44, 18, theme::kMuted);
  }
  ui_.text(tr("info.id") + " " + b.id, x + 22, yy, 18, theme::kMuted);
}

void App::drawInteriorInfo() {
  const Interior* in = insideInterior();
  if (!in) return;
  const float w = 560, x = ui_.vw() - w - 30, y = 20;
  ui_.panel({x, y, w, 290});
  float yy = y + 18;
  ui_.text(tr("interior.title"), x + 22, yy, 22, theme::kMuted);
  yy += 34;
  ui_.text(in->name(), x + 22, yy, 34, theme::kText);
  yy += 52;
  const auto g = world_.toGeodetic(player_.pos);
  const auto street = world_.terrainHeight(player_.pos.x, player_.pos.y);
  const double depth = street ? player_.pos.z - *street : 0.0;
  ui_.text(i18n_.f("interior.depth", {{"d", fixed(depth, 1)}, {"h", fixed(g.h_ellipsoidal_m, 1)}}), x + 22, yy, 24, theme::kText);
  yy += 38;
  ui_.text(tr("interior.badge"), x + 22, yy, 24, theme::kGood);
  yy += 36;
  yy += ui_.textWrapped(tr("interior.note"), x + 22, yy, w - 44, 20, theme::kWarn);
  ui_.textWrapped(tr("info.source") + "  " + tr("info.source_short.0"), x + 22, yy + 4, w - 44, 18, theme::kMuted);
}

void App::drawWalkerInfo() {
  const Walker& w = *hover_walker_;
  if (w.visitor) {
    const float w_ = 560, x = ui_.vw() - w_ - 30, y = 20;
    ui_.panel({x, y, w_, 200});
    float yy = y + 18;
    ui_.text(tr("npc.visitor_title"), x + 22, yy, 22, theme::kMuted);
    yy += 40;
    ui_.textWrapped(tr("npc.visitor_note"), x + 22, yy, w_ - 44, 20, theme::kWarn);
    return;
  }
  const auto& n = town_.npc(w.npc);
  const auto* occ = rj::sim::occupationById(n.occupation_id);
  const float w_ = 560, x = ui_.vw() - w_ - 30, y = 20;
  ui_.panel({x, y, w_, 300});
  float yy = y + 18;
  ui_.text(tr("npc.title"), x + 22, yy, 22, theme::kMuted);
  yy += 34;
  ui_.text(n.fullName() + "  " + i18n_.f("npc.age", {{"n", std::to_string(n.age)}}), x + 22, yy, 34, theme::kText);
  yy += 50;
  ui_.text(occ ? std::string(i18n_.code() == "ja" ? occ->name_ja : occ->name_en) : n.occupation_id, x + 22, yy, 26, theme::kText);
  yy += 40;
  ui_.text(i18n_.f("npc.walking_to", {{"next", tr("activity." + std::to_string(static_cast<int>(w.trip.next)))}}), x + 22, yy, 24, theme::kGood);
  yy += 36;
  const auto two2 = [](int v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  ui_.text(i18n_.f("npc.schedule", {{"a", two2(w.trip.start_min / 60) + ":" + two2(w.trip.start_min % 60)},
                                    {"b", two2(w.trip.end_min / 60) + ":" + two2(w.trip.end_min % 60)}}),
           x + 22, yy, 22, theme::kMuted);
  yy += 40;
  ui_.textWrapped(tr("npc.fictional"), x + 22, yy, w_ - 44, 20, theme::kWarn);
}

void App::drawPause() {
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 140});
  const float w = 520, x = (ui_.vw() - w) / 2;
  ui_.panel({x - 40, 220, w + 80, 640});
  ui_.textCentered(tr("menu.paused"), x + w / 2, 250, 44, theme::kText);
  float y = 330;
  const float h = 70, gap = 16;
  if (ui_.button({x, y, w, h}, tr("menu.resume"))) screen_ = Screen::Game;
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.save"))) {
    slots_saving_ = true;
    slots_return_ = Screen::Pause;
    screen_ = Screen::Slots;
  }
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.load"))) {
    slots_saving_ = false;
    slots_return_ = Screen::Pause;
    screen_ = Screen::Slots;
  }
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.settings"))) {
    settings_return_ = Screen::Pause;
    screen_ = Screen::Settings;
  }
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.to_title"))) endSession();
  y += h + gap;
  if (ui_.button({x, y, w, h}, tr("menu.quit"))) {
    saveSlot(kAutosaveSlot);
    quit_ = true;
  }
}

void App::drawMap(Rectangle r, double half, bool labels) {
  ui_.panel({r.x - 6, r.y - 6, r.width + 12, r.height + 12}, Color{0, 0, 0, 200});
  const Rectangle pr = ui_.px(r);
  BeginScissorMode(static_cast<int>(pr.x), static_cast<int>(pr.y), static_cast<int>(pr.width), static_cast<int>(pr.height));
  DrawRectangleRec(pr, Color{60, 60, 64, 255});
  const double ppm = pr.width / (2.0 * half);  // pixels per metre
  const float cx = pr.x + pr.width / 2, cy = pr.y + pr.height / 2;
  auto toScreen = [&](const rj::geo::Vec3d& p) {
    return Vector2{static_cast<float>(cx + (p.x - player_.pos.x) * ppm), static_cast<float>(cy - (p.y - player_.pos.y) * ppm)};
  };
  for (const auto& [code, c] : world_.cells()) {
    if (!c->gpu.ground.id) continue;
    const double* b = c->cpu->bounds;
    const Vector2 sw = toScreen(world_.toLocal({b[0], b[1], c->meta.h}));
    const Vector2 ne = toScreen(world_.toLocal({b[2], b[3], c->meta.h}));
    const Rectangle dst{sw.x, ne.y, ne.x - sw.x, sw.y - ne.y};
    DrawTexturePro(c->gpu.ground, {0, 0, static_cast<float>(c->gpu.ground.width), static_cast<float>(c->gpu.ground.height)}, dst, {0, 0}, 0, WHITE);
  }
  if (labels) {
    for (const auto& p : world_.meta().pois) {
      const Vector2 s = toScreen(world_.toLocal({p.lat, p.lon, 0}));
      if (!CheckCollisionPointRec(s, pr)) continue;
      DrawCircleV(s, 4 * ui_.scale(), theme::kAccent);
      if (half < 600) ui_.text(p.name, s.x / ui_.scale() + 8, s.y / ui_.scale() - 10, 18, WHITE);
    }
  }
  if (jobs_.active()) {  // job target
    const Vector2 s = toScreen(jobs_.target().pos);
    DrawCircleV(s, 7 * ui_.scale(), theme::kWarn);
    DrawCircleLinesV(s, 10 * ui_.scale(), BLACK);
  }
  // Player arrow (north-up map; yaw is the compass heading).
  const float a = player_.yaw;
  const float L = 14 * ui_.scale();
  const Vector2 tip{cx + std::sin(a) * L, cy - std::cos(a) * L};
  const Vector2 l{cx + std::sin(a - 2.5f) * L * 0.7f, cy - std::cos(a - 2.5f) * L * 0.7f};
  const Vector2 rr{cx + std::sin(a + 2.5f) * L * 0.7f, cy - std::cos(a + 2.5f) * L * 0.7f};
  DrawTriangle(tip, l, rr, theme::kAccent);
  DrawTriangle(tip, rr, l, theme::kAccent);
  EndScissorMode();
  ui_.text("N", r.x + r.width - 26, r.y + 6, 22, WHITE);
}

void App::drawPhone() {
  const float w = 480, h = 900, x = ui_.vw() - w - 40, y = 90;
  DrawRectangleRounded(ui_.px({x - 10, y - 10, w + 20, h + 20}), 0.08f, 12, Color{5, 5, 8, 245});
  DrawRectangleRounded(ui_.px({x, y, w, h}), 0.07f, 12, Color{24, 26, 34, 255});
  const auto t = jst();
  ui_.text(two(t.hour) + ":" + two(t.minute), x + 24, y + 16, 26, theme::kText);
  ui_.textRight(tr("phone.title"), x + w - 24, y + 18, 22, theme::kMuted);
  const float cx = x + 24, cw = w - 48;
  float yy = y + 70;
  auto back = [&]() {
    if (ui_.button({cx, y + h - 80, cw, 56}, tr("phone.home"), true, 28)) phone_app_ = PhoneApp::Home;
  };
  switch (phone_app_) {
    case PhoneApp::Home: {
      struct AppDef {
        const char* key;
        PhoneApp app;
        Color c;
      };
      const AppDef apps[] = {{"phone.map", PhoneApp::Map, Color{40, 140, 90, 255}},
                             {"phone.clock", PhoneApp::Clock, Color{60, 90, 190, 255}},
                             {"phone.wallet", PhoneApp::Wallet, Color{200, 140, 30, 255}},
                             {"phone.town", PhoneApp::Town, Color{170, 60, 150, 255}},
                             {"phone.work", PhoneApp::Work, Color{210, 80, 40, 255}},
                             {"phone.hobby", PhoneApp::Hobby, Color{40, 150, 170, 255}}};
      const float bw = (cw - 20) / 2, bh = 100;
      for (int i = 0; i < 6; ++i) {
        const Rectangle r{cx + (i % 2) * (bw + 20), yy + (i / 2) * (bh + 20), bw, bh};
        DrawRectangleRounded(ui_.px(r), 0.2f, 8, apps[i].c);
        if (ui_.hovered(r)) DrawRectangleRoundedLinesEx(ui_.px(r), 0.2f, 8, 3 * ui_.scale(), WHITE);
        ui_.textCentered(tr(apps[i].key), r.x + bw / 2, r.y + bh / 2 - 16, 30, WHITE);
        if (ui_.hovered(r) && ui_.clicked()) {
          ui_.consumeClick();
          phone_app_ = apps[i].app;
        }
      }
      yy += 3 * (bh + 20) + 10;
      if (ui_.button({cx, yy, cw, 64}, tr("phone.settings"), true, 28)) {
        settings_return_ = Screen::Phone;
        screen_ = Screen::Settings;
      }
      yy += 90;
      ui_.text(tr("phone.not_impl"), cx, yy, 26, theme::kWarn);
      yy += 40;
      ui_.textWrapped(tr("phone.not_impl_list"), cx, yy, cw, 22, theme::kMuted);
      ui_.textCentered(tr("phone.hint"), x + w / 2, y + h - 50, 20, theme::kMuted);
      break;
    }
    case PhoneApp::Work:
      drawPhoneWork(cx, yy, cw, y + h - 90);
      back();
      break;
    case PhoneApp::Hobby:
      drawPhoneHobby(cx, yy, cw, y + h - 90);
      back();
      break;
    case PhoneApp::Map: {
      ui_.text(tr("phone.map"), cx, yy, 32, theme::kText);
      yy += 50;
      drawMap({cx, yy, cw, cw}, map_half_extent_, true);
      yy += cw + 20;
      ui_.textWrapped(tr("phone.map_hint"), cx, yy, cw, 20, theme::kMuted);
      back();
      break;
    }
    case PhoneApp::Clock: {
      ui_.text(tr("phone.clock"), cx, yy, 32, theme::kText);
      yy += 60;
      ui_.text(two(t.hour) + ":" + two(t.minute), cx, yy, 96, theme::kText);
      yy += 120;
      ui_.text(i18n_.f("phone.clock_date", {{"y", std::to_string(t.date.y)}, {"m", std::to_string(t.date.m)},
                                            {"d", std::to_string(t.date.d)},
                                            {"w", tr("weekday." + std::to_string(rj::sim::weekday(t.date)))}}),
               cx, yy, 28, theme::kText);
      yy += 44;
      if (auto hol = rj::sim::holidayName(t.date)) {
        ui_.text(i18n_.f("phone.clock_holiday", {{"name", std::string(*hol)}}), cx, yy, 26, theme::kWarn);
        yy += 40;
      }
      ui_.text(i18n_.f("phone.clock_season", {{"s", tr("season." + std::to_string(static_cast<int>(rj::sim::season(t.date))))}}), cx, yy, 26, theme::kText);
      yy += 44;
      const auto g = world_.toGeodetic(player_.pos);
      const auto sun = rj::env::sunPosition(clock_.unixUtc(), g.lat_deg, g.lon_deg);
      yy += ui_.textWrapped(i18n_.f("phone.clock_sun", {{"el", fixed(sun.elevation_deg, 1)}, {"az", fixed(sun.azimuth_deg, 0)}}), cx, yy, cw, 24, theme::kText) + 12;
      ui_.textWrapped(tr("phone.clock_weather"), cx, yy, cw, 22, theme::kWarn);
      back();
      break;
    }
    case PhoneApp::Wallet: {
      ui_.text(tr("phone.wallet"), cx, yy, 32, theme::kText);
      yy += 60;
      const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
      ui_.text(i18n_.f("phone.wallet_balance", {{"n", withCommas(bal)}}), cx, yy, 44, theme::kText);
      yy += 80;
      ui_.text(tr("phone.wallet_history"), cx, yy, 26, theme::kMuted);
      yy += 40;
      if (ledger_) {
        const auto& j = ledger_->journal();
        int shown = 0;
        for (auto it = j.rbegin(); it != j.rend() && shown < 8; ++it, ++shown) {
          const bool in = it->to == player_account_;
          ui_.text((in ? "+¥" : "-¥") + withCommas(it->amount), cx, yy, 24, in ? theme::kGood : theme::kWarn);
          ui_.text(it->memo, cx + 170, yy + 2, 20, theme::kMuted);
          yy += 36;
        }
      }
      yy += 20;
      ui_.textWrapped(tr("phone.wallet_note"), cx, yy, cw, 22, theme::kWarn);
      back();
      break;
    }
    case PhoneApp::Town: {
      ui_.text(tr("phone.town"), cx, yy, 32, theme::kText);
      yy += 50;
      ui_.text(i18n_.f("phone.town_title", {{"n", std::to_string(town_.size() - town_.commuters())},
                                             {"c", std::to_string(town_.commuters())}}), cx, yy, 22, theme::kMuted);
      yy += 40;
      const auto hist = town_.histogram(t.date, t.minuteOfDay());
      const int total = std::max<int>(1, static_cast<int>(town_.size()));
      for (size_t i = 0; i < hist.size(); ++i) {
        if (hist[i] == 0) continue;
        ui_.text(tr("activity." + std::to_string(i)), cx, yy, 22, theme::kText);
        const float bw = (cw - 200) * static_cast<float>(hist[i]) / static_cast<float>(total);
        DrawRectangleRec(ui_.px({cx + 170, yy + 4, std::max(2.0f, bw), 20}), Color{170, 60, 150, 255});
        ui_.textRight(std::to_string(hist[i]), cx + cw, yy, 22, theme::kMuted);
        yy += 32;
      }
      yy += 12;
      ui_.textWrapped(tr("phone.town_note"), cx, yy, cw, 20, theme::kWarn);
      back();
      break;
    }
  }
}

void App::drawToast(float dt) {
  if (toast_t_ <= 0 || !ui_.hasFont()) return;
  toast_t_ -= dt;
  const float a = std::clamp(toast_t_, 0.0f, 1.0f);
  const float w = ui_.measure(toast_, 28) + 60;
  ui_.panel({(ui_.vw() - w) / 2, 940, w, 56}, Color{0, 0, 0, static_cast<unsigned char>(200 * a)});
  ui_.textCentered(toast_, ui_.vw() / 2, 952, 28, Color{255, 255, 255, static_cast<unsigned char>(255 * a)});
}

}  // namespace rjc
