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
    const bool walking = player_.auto_forward_s > 0.0f || !walk_legs_.empty();  // scripted walk finishes first
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
  slice_dir_ = data / "world" / "shibuya";
  std::string err;
  if (!world_.loadMeta(slice_dir_, err)) {
    fatal_ = "World data error: " + err;
    return false;
  }
  town_.load(slice_dir_ / "residents.csv");
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

void App::saveSettings() { settings_.save(userDir() / "settings.ini"); }

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

// ---------------------------------------------------------------------------
void App::update(float dt) {
  ui_.beginFrame();
  if (screen_ == Screen::Boot) return;  // boot happens after the first frame is shown
  if (screen_ == Screen::Fatal) return;

  if (IsKeyPressed(KEY_F12)) takeUserScreenshot();

  // Stream the world around the current viewpoint.
  rj::geo::Vec3d focus = (in_session_ ? player_.pos : rj::geo::Vec3d{0, 0, 0});
  if (world_.update(focus, settings_.view_distance_m)) {
    if (in_session_) player_.pos = focus;
    facades_.clear();  // facade meshes live in origin coordinates
  }
  if (in_session_ && screen_ != Screen::Loading) facades_.update(world_, player_.pos, opt_.screenshot.empty() ? 6 : 60);
  if (frame_ % 60 == 0) signals_.rebuild(world_);
  signals_.update(GetTime());

  if (screen_ == Screen::Loading) {
    if (world_.residentCount() >= world_.knownCount() || (world_.pendingJobs() == 0 && world_.residentCount() > 0 && frame_ > 600)) {
      player_.snapToGround(world_);
      peds_.buildNav(world_);
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
          if (st.rfind("phone", 0) == 0) {
            screen_ = Screen::Phone;
            if (st == "phone:map") phone_app_ = PhoneApp::Map;
            if (st == "phone:town") phone_app_ = PhoneApp::Town;
            if (st == "phone:clock") phone_app_ = PhoneApp::Clock;
            if (st == "phone:wallet") phone_app_ = PhoneApp::Wallet;
          }
        }
        if (!opt_.screenshot.empty()) shot_frames_ = opt_.frames;
      } else if (!opt_.screenshot.empty()) {
        shot_frames_ = opt_.frames;
      }
    }
    return;
  }

  const bool want_lock = (screen_ == Screen::Game);
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
      player_.update(dt, world_, settings_, screen_ == Screen::Game, insideInterior(), nearby);
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
      peds_.update(town_, world_, clock_.jst(), player_.pos, dt);
      if (screen_ == Screen::Game) {
        updateInteriorAction();
        if (inside_id_.empty()) {
          hover_ = world_.pick(player_.eyeEnu(), player_.forwardEnu(), 300.0);
          hover_walker_ =
              peds_.pick(player_.eyeEnu(), player_.forwardEnu(), hover_ ? std::min(40.0, hover_->distance) : 40.0);
        } else {
          hover_.reset();
          hover_walker_ = nullptr;
        }
        if (IsKeyPressed(KEY_ESCAPE)) screen_ = Screen::Pause;
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

void App::runSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    TraceLog(ok ? LOG_INFO : LOG_ERROR, "RJ: SELFTEST %s %s", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  check(world_.residentCount() == world_.knownCount(), "all world cells loaded");
  check(world_.buildingCount() > 9000, "buildings present (PLATEAU)");
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
  // Verified interior: enter through a real entrance, stand on a real floor, walls block.
  check(!world_.meta().interiors.empty(), "interior listed in slice");
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
  std::vector<rj::geo::Vec3d> shops;
  facades_.collectLights(c, 90.0, shops);  // lit shop fronts spill onto the sidewalk
  for (const auto& p : shops) {
    const double dx = p.x - c.x, dy = p.y - c.y;
    cand.push_back({dx * dx + dy * dy + 400.0, {enuToRl(p), 9.0f, Vector3Scale(Vector3{1.0f, 0.93f, 0.82f}, 7.0f * std::max(k, 0.25f) * lighting_.occupancy.z)}});
  }
  std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
  for (size_t i = 0; i < cand.size() && out.size() < 24; ++i) out.push_back(cand[i].l);
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
    std::vector<const Mesh*> casters;
    facades_.forEachMesh([&](const Mesh& m, int lod) {
      if (lod == 0) casters.push_back(&m);
    });
    renderer_.renderShadowMaps(cam, world_, lighting_, casters);
  }
  // Indoors walls can be 0.3 m from the eye: pull the near plane in so it never cuts through them.
  Renderer::setClipPlanes(in ? 0.08f : 0.2f, static_cast<float>(settings_.view_distance_m) * 1.6f + 500.0f);
  renderer_.beginScene(ro, lighting_, cam, render_time_);
  ClearBackground(deep ? Color{58, 58, 60, 255} : BLACK);
  const float aspect = static_cast<float>(GetScreenWidth()) / static_cast<float>(std::max(1, GetScreenHeight()));
  if (!deep) renderer_.drawSky(cam, lighting_, aspect);
  renderer_.setLights(collectLights(cam));
  BeginMode3D(cam);
  renderer_.drawWorld(cam, world_, settings_.photo_textures, !in);
  static const bool no_facades = std::getenv("RJ_NO_FACADES") != nullptr;      // debug isolation
  static const bool no_int_out = std::getenv("RJ_NO_INTERIOR_OUT") != nullptr;
  if (!deep) {
    if (!no_facades) renderer_.drawFacades(facades_);
    renderer_.drawSignals(signals_, cam);
  }
  if (in) {
    renderer_.drawInterior(*in);
  } else if (in_session_ && screen_ != Screen::Title && !no_int_out) {
    // From the street the stairs are visible through the real openings cut into the pavement.
    for (const auto& [id, interior] : world_.interiors()) renderer_.drawInterior(*interior);
  }
  if (in_session_ && screen_ != Screen::Title && !deep) renderer_.drawPedestrians(peds_);
  if (in_session_ && player_.camera_mode == 1 && screen_ != Screen::Title) renderer_.drawPlayerBody(enuToRl(player_.pos), player_.yaw);
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
  drawWorldView(game_view ? player_.camera(settings_.fov, third_dist) : titleCamera());

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
  ui_.textRight("v0.1.0  ·  " + std::to_string(world_.buildingCount()) + " buildings (PLATEAU)", vw - 30, 1040, 20, theme::kMuted);
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
  const auto g = world_.toGeodetic(player_.pos);
  const auto t = jst();
  const int cx = GetScreenWidth() / 2, cy = GetScreenHeight() / 2;
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
  row(tr("info.lod"), tr(b.lod >= 2 ? "info.lod2" : "info.lod1"));
  yy += 6;
  ui_.text(tr(b.geometry_status == 0 ? "verify.badge.verified_exterior" : "verify.badge.unverified"), x + 22, yy, 24,
           b.geometry_status == 0 ? theme::kGood : theme::kWarn);
  yy += 34;
  yy += ui_.textWrapped(tr("verify.interior.unknown"), x + 22, yy, w - 44, 22, theme::kWarn);
  yy += 4;
  yy += ui_.textWrapped(tr("info.source") + "  " + tr("info.source_short." + std::to_string(b.source_index)), x + 22, yy,
                        w - 44, 18, theme::kMuted);
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
                             {"phone.town", PhoneApp::Town, Color{170, 60, 150, 255}}};
      const float bw = (cw - 20) / 2, bh = 120;
      for (int i = 0; i < 4; ++i) {
        const Rectangle r{cx + (i % 2) * (bw + 20), yy + (i / 2) * (bh + 20), bw, bh};
        DrawRectangleRounded(ui_.px(r), 0.2f, 8, apps[i].c);
        if (ui_.hovered(r)) DrawRectangleRoundedLinesEx(ui_.px(r), 0.2f, 8, 3 * ui_.scale(), WHITE);
        ui_.textCentered(tr(apps[i].key), r.x + bw / 2, r.y + bh / 2 - 16, 30, WHITE);
        if (ui_.hovered(r) && ui_.clicked()) {
          ui_.consumeClick();
          phone_app_ = apps[i].app;
        }
      }
      yy += 2 * (bh + 20) + 10;
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
