// Work and hobbies: taxi / delivery jobs, train driving, the shop-till minigame, simple shifts,
// fishing, photography and visiting the shrine / temple. Part of App (see app.hpp).
// Game values (pay, fish, fortunes) are labelled as such in the UI; nothing here claims to be a
// statistic.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.hpp"
#include "platform/paths.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "world/coords.hpp"

namespace rjc {
namespace {
using V3 = rj::geo::Vec3d;
std::string yen(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return (v < 0 ? "-" : "") + s;
}
std::string distStr(double m) {
  char b[32];
  if (m >= 1000) std::snprintf(b, sizeof b, "%.1f km", m / 1000.0);
  else std::snprintf(b, sizeof b, "%d m", static_cast<int>(m));
  return b;
}
uint32_t xr(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
double frand(uint32_t& s) { return (xr(s) & 0xffffff) / 16777216.0; }

// Shop till: items with typical convenience-store prices (tax included, game values).
struct TillItem {
  const char* key;
  int price;
  bool age;  // needs the age check (alcohol)
};
const TillItem kTill[] = {{"till.item.onigiri_salmon", 162, false}, {"till.item.onigiri_tuna", 151, false}, {"till.item.tea", 151, false},
                          {"till.item.coffee", 140, false},         {"till.item.bento", 594, false},        {"till.item.sandwich", 356, false},
                          {"till.item.karaage", 248, false},        {"till.item.nikuman", 184, false},      {"till.item.cupnoodle", 237, false},
                          {"till.item.pudding", 213, false},        {"till.item.water", 108, false},        {"till.item.beer", 280, true},
                          {"till.item.magazine", 690, false},       {"till.item.pen", 165, false}};
constexpr int kTillN = sizeof(kTill) / sizeof(kTill[0]);
constexpr int64_t kTillHourly = 1250;  // around Tokyo's minimum wage (game value)

// Fish of a Tokyo-Bay-like coast in early autumn (game values: weight, size range cm, strength).
struct FishDef {
  const char* key;
  float weight, min_cm, max_cm, strength;
  bool night;
};
const FishDef kFish[] = {{"fish.haze", 30, 8, 18, 0.6f, false},     {"fish.aji", 22, 14, 30, 0.9f, false}, {"fish.iwashi", 14, 10, 20, 0.6f, false},
                         {"fish.saba", 10, 20, 38, 1.3f, false},    {"fish.kisu", 9, 12, 24, 0.8f, false}, {"fish.mebaru", 6, 12, 25, 1.0f, true},
                         {"fish.kasago", 6, 12, 24, 1.0f, true},    {"fish.kurodai", 2, 25, 48, 1.6f, false}, {"fish.suzuki", 1.5f, 40, 78, 1.9f, true}};
constexpr int kFishN = sizeof(kFish) / sizeof(kFish[0]);

// Omikuji (shrine fortunes): proportions differ between shrines; these are game values.
const char* kOmikuji[] = {"omikuji.daikichi", "omikuji.kichi", "omikuji.chukichi", "omikuji.shokichi", "omikuji.suekichi", "omikuji.kyo"};
const int kOmikujiW[] = {16, 25, 20, 15, 14, 10};
}  // namespace

// ------------------------------------------------------------------------------------------------
bool App::waterAhead(double& sea_z, V3& spot) const {
  const auto g = world_.toGeodetic(player_.pos);
  sea_z = world_.toLocal({g.lat_deg, g.lon_deg, 0.0}).z;
  if (player_.pos.z - sea_z > 9.0) return false;
  const double fx = std::sin(player_.yaw), fy = std::cos(player_.yaw);
  for (double d : {3.0, 5.0, 8.0}) {
    const V3 q{player_.pos.x + fx * d, player_.pos.y + fy * d, sea_z};
    const auto h = world_.terrainHeight(q.x, q.y);
    const bool deck = world_.roadHeight(q.x, q.y).has_value() && world_.roadHeight(q.x, q.y).value() > sea_z + 0.5;
    if ((!h || *h < sea_z - 0.4) && !deck) {
      spot = {player_.pos.x + fx * (d + 9.0), player_.pos.y + fy * (d + 9.0), sea_z + 0.02};
      return true;
    }
  }
  return false;
}

Camera3D App::cabCamera() const {
  Camera3D c{};
  const Train* t = trains_.train(ride_train_);
  if (!t) return player_.camera(settings_.fov);
  rj::geo::Vec3d p;
  float yaw, pitch;
  trains_.carPose(*t, 0, p, yaw, pitch);
  const double fx = std::sin(yaw), fy = std::cos(yaw), lx = -fy, ly = fx;
  const double fwd = t->car_len * 0.5 - 1.1;
  const rj::geo::Vec3d eye{p.x + fx * fwd + lx * 0.55, p.y + fy * fwd + ly * 0.55, p.z + kCommuterFloorZ + 1.35 + std::tan(pitch) * fwd};
  const float y = yaw + ride_look_yaw_, pt = pitch + ride_look_pitch_;
  c.position = enuToRl(eye);
  c.target = enuToRl({eye.x + std::sin(y) * std::cos(pt), eye.y + std::cos(y) * std::cos(pt), eye.z + std::sin(pt)});
  c.up = {0, 1, 0};
  c.fovy = settings_.fov;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

void App::startTrainDriving() {
  if (!trains_.loaded()) return;
  // riding a stopped loop-line train, or on a loop-line platform beside one
  int tid = -1;
  if (ride_train_ >= 0) {
    const Train* t = trains_.train(ride_train_);
    if (t && t->at_station >= 0 && trains_.lines()[static_cast<size_t>(t->line)].kind == LineKind::Loop) tid = t->id;
  } else {
    const int si = trains_.stationNear(player_.pos, 190.0);
    if (si >= 0 && trains_.stations()[static_cast<size_t>(si)].line == 0 &&
        std::fabs(player_.pos.z - trains_.stations()[static_cast<size_t>(si)].pos.z) < 2.5)
      for (const auto& t : trains_.trains())
        if (t.at_station == si && t.dwell > 3.0) tid = t.id;
  }
  if (tid < 0) {
    toast(tr("job.train.where"));
    return;
  }
  ride_train_ = tid;
  ride_car_ = 0;
  drive_train_ = tid;
  ride_look_yaw_ = 0.0f;
  ride_look_pitch_ = -0.08f;
  trains_.setManual(tid, true);
  train_stops_ = 0;
  train_pay_ = 0;
  screen_ = Screen::Game;
  toast(tr("job.train.started"));
}

void App::updateTrainDriving() {
  const Train* t = trains_.train(drive_train_);
  if (!t) {
    drive_train_ = -1;
    return;
  }
  if (opt_.state == "trainjob" && ride_test_t_ >= 0) {  // test aid: depart under power, then coast
    trains_.setNotch(t->id, ride_test_t_ < opt_.ride * 0.75f ? 4 : 0);
    return;
  }
  if (screen_ == Screen::Game) {
    int n = t->notch;
    if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP)) n = std::min(5, n <= -8 ? -7 : n + 1);
    if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN)) n = std::max(-7, n - 1);
    if (IsKeyPressed(KEY_SPACE)) n = -8;  // emergency brake
    if (n != t->notch) trains_.setNotch(t->id, n);
    if (IsKeyPressed(KEY_E)) {
      if (t->at_station >= 0) {  // doors open: hand the train back and ride on as a passenger
        trains_.setManual(t->id, false);
        drive_train_ = -1;
        ride_car_ = t->cars / 2;
        toast(i18n_.f("job.train.ended", {{"n", std::to_string(train_stops_)}, {"pay", yen(train_pay_)}}));
        return;
      }
      double err = 0;
      if (trains_.openDoors(t->id, err)) {
        ++train_stops_;
        const double a = std::fabs(err);
        int64_t pay = 400 + (a <= 0.5 ? 300 : a <= 2.0 ? 150 : 0);  // game values
        train_pay_ += pay;
        if (ledger_) ledger_->transfer(ledger_->externalAccount(), player_account_, pay, rj::econ::TxCategory::Wage, clock_.unixUtc(), tr("job.train.name"));
        char b[16];
        std::snprintf(b, sizeof b, "%+.1f", err);
        train_msg_ = i18n_.f(a <= 0.5 ? "job.train.stop_great" : a <= 2.0 ? "job.train.stop_good" : "job.train.stop_ok", {{"err", b}, {"pay", yen(pay)}});
        train_msg_t_ = 5.0f;
      } else if (t->v < 0.1) {
        train_msg_ = tr("job.train.not_at_mark");
        train_msg_t_ = 3.0f;
      }
    }
  }
}

void App::tillNextCustomer() {
  till_.items.clear();
  till_.scanned.clear();
  const int n = 1 + static_cast<int>(xr(till_.rng) % 5);
  for (int i = 0; i < n; ++i) till_.items.push_back(static_cast<int>(xr(till_.rng) % kTillN));
  till_.scanned.assign(till_.items.size(), false);
  till_.stage = 0;
  till_.age_checked = false;
  till_.entered = 0;
  int64_t total = 0;
  for (int k : till_.items) total += kTill[k].price;
  // customers pay exactly, with the next 1,000-yen note, or with a 5,000 / 10,000 note
  const int r = static_cast<int>(xr(till_.rng) % 10);
  till_.tendered = r < 2 ? total : r < 7 ? ((total + 999) / 1000) * 1000 : total < 5000 && r < 9 ? 5000 : 10000;
  if (till_.tendered < total) till_.tendered = ((total + 999) / 1000) * 1000;
}

void App::takePhoto() {
  // save the frame (without the HUD) and check the photo spots in view
  Image img = LoadImageFromScreen();
  const auto dir = userDir() / "photos";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto t = jst();
  char name[64];
  std::snprintf(name, sizeof name, "photo_%04d%02d%02d_%02d%02d%02d_%d.png", t.date.y, t.date.m, t.date.d, t.hour, t.minute, t.second, photos_taken_);
  ExportImage(img, pathToUtf8(dir / name).c_str());
  UnloadImage(img);
  ++photos_taken_;
  const Camera3D cam = player_.camera(photo_fov_ > 0 ? photo_fov_ : settings_.fov);
  const V3 eye = rlToEnu(cam.position), tgt = rlToEnu(cam.target);
  const V3 f{tgt.x - eye.x, tgt.y - eye.y, tgt.z - eye.z};
  const double fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
  std::string got;
  for (const auto& p : world_.meta().pois) {
    if (p.usage != 454 && p.usage != 431 && p.usage != 422) continue;
    const V3 q = world_.toLocal({p.lat, p.lon, p.height * 0.5});
    const V3 d{q.x - eye.x, q.y - eye.y, q.z - eye.z};
    const double dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl > 1600.0 || dl < 5.0) continue;
    const double c = (d.x * f.x + d.y * f.y + d.z * f.z) / (dl * fl);
    if (c > std::cos((photo_fov_ > 0 ? photo_fov_ : settings_.fov) * 0.5 * DEG2RAD * 0.8) && !photo_spots_.count(p.name)) {
      photo_spots_.insert(p.name);
      got = p.name;
      break;
    }
  }
  if (!got.empty()) toast(i18n_.f("photo.spot", {{"name", got}, {"n", std::to_string(photo_spots_.size())}}));
  else toast(i18n_.f("photo.saved", {{"file", name}}));
}

// ------------------------------------------------------------------------------------------------
void App::updateActivities(float dt) {
  const int hour = jst().hour;
  // jobs
  if (jobs_.active()) {
    const bool in_taxi = driving_.active() && driving_.car().type == VehicleType::Taxi;
    const double spd = driving_.active() ? std::fabs(driving_.car().v) : 0.0;
    for (const auto& e : jobs_.update(dt, world_, traffic_, player_.pos, spd, in_taxi, driving_.active() || flying_, hour)) {
      if (e.pay > 0 && ledger_)
        ledger_->transfer(ledger_->externalAccount(), player_account_, e.pay, rj::econ::TxCategory::Wage, clock_.unixUtc(), tr("phone.work"));
      toast(i18n_.f(e.key, std::vector<std::pair<std::string, std::string>>(e.args.begin(), e.args.end())));
    }
  }
  if (train_msg_t_ > 0) train_msg_t_ -= dt;
  if (till_.msg_t > 0) till_.msg_t -= dt;
  if (drive_train_ >= 0) updateTrainDriving();
  if (screen_ != Screen::Game) return;
  // photo mode: zoom with the wheel, E takes a picture, Esc / Tab leaves
  if (photo_mode_) {
    const float wheel = GetMouseWheelMove();
    if (photo_fov_ <= 0) photo_fov_ = settings_.fov;
    photo_fov_ = std::clamp(photo_fov_ - wheel * 4.0f, 18.0f, 80.0f);
    if (IsKeyPressed(KEY_E)) photo_request_ = true;
    prompt_.clear();
    return;
  }
  // worship sequence (shrine: bow twice, clap twice, bow once; temple: palms together, bow)
  if (worship_.stage > 0) {
    worship_.t += dt;
    const double durs[] = {0, 1.0, 2.4, worship_.temple ? 1.6 : 1.4, 1.3};
    const float bow = static_cast<float>(worship_.stage == 2 || worship_.stage == 4 ? std::sin(std::fmod(worship_.t, 1.2) / 1.2 * PI) * 0.55 : 0.0);
    player_.pitch = worship_.pitch0 - bow;
    prompt_ = tr(worship_.stage == 1 ? "worship.coin" : worship_.stage == 2 ? "worship.bow2" : worship_.stage == 3 ? (worship_.temple ? "worship.gassho" : "worship.clap2") : "worship.bow1");
    if (worship_.t > durs[worship_.stage]) {
      worship_.t = 0;
      if (++worship_.stage > 4) {
        worship_.stage = 0;
        player_.pitch = worship_.pitch0;
        ++worship_count_;
        toast(i18n_.f("worship.done", {{"place", worship_.place}}));
      }
    }
    return;
  }
  // fishing
  if (fish_.stage > 0) {
    fish_.t += dt;
    const bool hold = IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    if (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ESCAPE)) {
      fish_.stage = 0;
      toast(tr("fish.stop"));
    } else if (fish_.stage == 1) {  // waiting for a bite
      prompt_ = tr("fish.waiting");
      if (fish_.t > fish_.wait) {
        fish_.stage = 2;
        fish_.t = 0;
      }
    } else if (fish_.stage == 2) {  // strike within 1.5 s
      prompt_ = tr("fish.bite");
      if (IsKeyPressed(KEY_SPACE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        // which fish: weights by time of day
        float tot = 0;
        const bool night = hour >= 18 || hour < 5;
        const bool mag = (hour >= 5 && hour < 8) || (hour >= 16 && hour < 19);  // dawn / dusk
        float w[kFishN];
        for (int i = 0; i < kFishN; ++i) tot += (w[i] = kFish[i].weight * (kFish[i].night ? (night ? 2.0f : 0.5f) : 1.0f) * (mag ? 1.3f : 1.0f));
        float r = static_cast<float>(frand(fish_.rng)) * tot;
        int s = 0;
        while (s < kFishN - 1 && (r -= w[s]) > 0) ++s;
        fish_.species = s;
        const float u = static_cast<float>(std::pow(frand(fish_.rng), 1.6));
        fish_.size = kFish[s].min_cm + (kFish[s].max_cm - kFish[s].min_cm) * u;
        fish_.strength = kFish[s].strength * (0.8 + 0.5 * u);
        fish_.dist = 10.0 + 10.0 * frand(fish_.rng);
        fish_.tension = 0.3;
        fish_.stage = 3;
        fish_.t = 0;
      } else if (fish_.t > 1.5) {
        toast(tr("fish.missed"));
        fish_.stage = 1;
        fish_.t = 0;
        fish_.wait = 6.0 + 20.0 * frand(fish_.rng);
      }
    } else if (fish_.stage == 3) {  // reel: keep the line tension in the safe band
      prompt_ = tr("fish.reel");
      const double pull = fish_.strength * (0.7 + 0.6 * std::sin(fish_.t * 1.7 + fish_.species));
      if (hold) {
        fish_.dist -= (1.5 - 0.35 * pull) * dt;
        fish_.tension += (0.28 * pull + 0.05) * dt;
      } else {
        fish_.dist += 0.45 * pull * dt;
        fish_.tension -= 0.45 * dt;
      }
      fish_.tension = std::max(0.0, fish_.tension);
      if (fish_.tension > 1.0) {
        toast(tr("fish.broke"));
        fish_.stage = 0;
      } else if (fish_.dist > 30.0) {
        toast(tr("fish.escaped"));
        fish_.stage = 0;
      } else if (fish_.dist <= 0.0) {
        const std::string key = kFish[fish_.species].key;
        char b[16];
        std::snprintf(b, sizeof b, "%.1f", fish_.size);
        ++fish_count_;
        const bool best = fish_.size > fish_log_[key];
        fish_log_[key] = std::max(fish_log_[key], fish_.size);
        toast(i18n_.f(best ? "fish.caught_best" : "fish.caught", {{"fish", tr(key)}, {"cm", b}}));
        fish_.stage = 0;
      }
    }
    return;
  }
  if (!prompt_.empty() || driving_.active() || flying_ || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || !inside_id_.empty()) return;
  const bool e = IsKeyPressed(KEY_E);
  // shrine / temple
  for (const auto& p : world_.meta().pois) {
    const bool shrine = p.name.find("神社") != std::string::npos, temple = p.name.find("寺") != std::string::npos && !shrine;
    if (!shrine && !temple) continue;
    const V3 q = world_.toLocal({p.lat, p.lon, 0.0});
    if (std::hypot(q.x - player_.pos.x, q.y - player_.pos.y) > 45.0) continue;
    prompt_ = i18n_.f(temple ? "worship.prompt_temple" : "worship.prompt", {{"place", p.name}});
    const bool o = IsKeyPressed(KEY_O), g = IsKeyPressed(KEY_G);
    if (e && ledger_ && ledger_->transfer(player_account_, ledger_->externalAccount(), 100, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("worship.offering")) == rj::econ::TxResult::Ok) {
      worship_ = {};
      worship_.stage = 1;
      worship_.temple = temple;
      worship_.place = p.name;
      worship_.pitch0 = player_.pitch;
    } else if (o && ledger_ && ledger_->transfer(player_account_, ledger_->externalAccount(), 100, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("worship.omikuji")) == rj::econ::TxResult::Ok) {
      uint32_t s = static_cast<uint32_t>(clock_.unixUtc()) * 2654435761u | 1u;
      int tot = 0;
      for (int w : kOmikujiW) tot += w;
      int r = static_cast<int>(xr(s) % static_cast<uint32_t>(tot)), k = 0;
      while ((r -= kOmikujiW[k]) >= 0) ++k;
      last_omikuji_ = kOmikuji[k];
      toast(i18n_.f("worship.omikuji_result", {{"r", tr(kOmikuji[k])}}));
    } else if (g && !goshuin_.count(p.name) && ledger_ &&
               ledger_->transfer(player_account_, ledger_->externalAccount(), 500, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("worship.goshuin")) == rj::econ::TxResult::Ok) {
      goshuin_.insert(p.name);
      toast(i18n_.f("worship.goshuin_got", {{"place", p.name}, {"n", std::to_string(goshuin_.size())}}));
    }
    return;
  }
  // fishing spot: water right in front (piers, quays, the shore)
  double sea_z;
  V3 spot;
  if (!player_.fly && waterAhead(sea_z, spot)) {
    prompt_ = tr("fish.prompt");
    if (e) {
      fish_.rng ^= static_cast<uint32_t>(clock_.unixUtc()) | 1u;
      fish_.stage = 1;
      fish_.t = 0;
      fish_.wait = 5.0 + 22.0 * frand(fish_.rng);
      fish_.bobber = spot;
    }
  }
}

// ------------------------------------------------------------------------------------------------
void App::drawWorldMarkers(const Camera3D& cam) {
  (void)cam;
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, false);
  if (jobs_.active()) {
    const V3 t = jobs_.target().pos;
    const Vector3 base = enuToRl({t.x, t.y, t.z});
    const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(GetTime()) * 3.0f);
    DrawCylinder(base, 0.9f, 0.9f, 40.0f, 16, Color{255, 196, 0, static_cast<unsigned char>(60 * pulse + 30)});
    DrawCylinderWires(base, 0.95f, 0.95f, 0.25f, 16, Color{255, 220, 60, 200});
  }
  if (fish_.stage > 0) {
    const float bob = fish_.stage == 2 ? -0.08f * std::fabs(std::sin(static_cast<float>(GetTime()) * 14.0f)) : 0.02f * std::sin(static_cast<float>(GetTime()) * 2.0f);
    V3 b = fish_.bobber;
    if (fish_.stage == 3) {  // the float comes in as the line is reeled
      const double d0 = std::hypot(b.x - player_.pos.x, b.y - player_.pos.y);
      const double k = std::clamp(fish_.dist / std::max(1.0, d0), 0.05, 1.0);
      b = {player_.pos.x + (b.x - player_.pos.x) * k, player_.pos.y + (b.y - player_.pos.y) * k, b.z};
    }
    const Vector3 bp = enuToRl({b.x, b.y, b.z + bob});
    DrawSphere(bp, 0.06f, Color{230, 40, 30, 255});
    DrawSphere({bp.x, bp.y + 0.06f, bp.z}, 0.045f, WHITE);
    const V3 tip{player_.pos.x + std::sin(player_.yaw) * 1.6, player_.pos.y + std::cos(player_.yaw) * 1.6, player_.pos.z + 2.3};
    DrawLine3D(enuToRl(tip), {bp.x, bp.y + 0.08f, bp.z}, Color{220, 220, 220, 160});
  }
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, true);
  if (jobs_.active() && jobs_.kind() == JobKind::Taxi && jobs_.stage() == 0) {  // the passenger waiting at the kerb
    const V3 t = jobs_.target().pos;
    const float yaw = static_cast<float>(std::atan2(player_.pos.x - t.x, player_.pos.y - t.y));
    renderer_.drawPlayerBody(enuToRl(t), yaw);
  }
}

// ------------------------------------------------------------------------------------------------
void App::drawActivityHud() {
  const float vw = ui_.vw();
  if (screen_ != Screen::Game && screen_ != Screen::Phone) return;
  // job status with a direction arrow
  if (jobs_.active()) {
    const JobPlace& t = jobs_.target();
    const double d = jobs_.distanceTo(player_.pos);
    std::string line;
    if (jobs_.kind() == JobKind::Taxi) {
      line = jobs_.stage() == 0 ? i18n_.f("job.taxi.hud_pickup", {{"d", distStr(d)}})
                                : i18n_.f("job.taxi.hud_ride", {{"dest", t.name.empty() ? std::string("-") : t.name}, {"d", distStr(d)}, {"fare", yen(jobs_.meterYen(jst().hour))}});
    } else {
      const int left = static_cast<int>(std::max(0.0, jobs_.timeLeft()));
      char mmss[16];
      std::snprintf(mmss, sizeof mmss, "%d:%02d", left / 60, left % 60);
      line = jobs_.stage() == 0 ? i18n_.f("job.delivery.hud_pickup", {{"shop", t.name.empty() ? tr("job.delivery.shop") : t.name}, {"d", distStr(d)}})
                                : i18n_.f("job.delivery.hud_drop", {{"d", distStr(d)}, {"t", mmss}});
    }
    const float w = ui_.measure(line, 28) + 120;
    ui_.panel({(vw - w) / 2, 20, w, 64}, Color{0, 0, 0, 170});
    ui_.text(line, (vw - w) / 2 + 90, 36, 28, theme::kText);
    // arrow relative to the view
    const double brg = std::atan2(t.pos.x - player_.pos.x, t.pos.y - player_.pos.y);
    const float a = static_cast<float>(brg - player_.yaw);
    const float ax = ((vw - w) / 2 + 45) * ui_.scale(), ay = 52 * ui_.scale(), L = 20 * ui_.scale();
    const Vector2 tip{ax + std::sin(a) * L, ay - std::cos(a) * L}, l{ax + std::sin(a - 2.5f) * L * 0.8f, ay - std::cos(a - 2.5f) * L * 0.8f},
        r{ax + std::sin(a + 2.5f) * L * 0.8f, ay - std::cos(a + 2.5f) * L * 0.8f};
    DrawTriangle(tip, l, r, theme::kWarn);
    DrawTriangle(tip, r, l, theme::kWarn);
  }
  // train driving
  if (drive_train_ >= 0) {
    if (const Train* t = trains_.train(drive_train_)) {
      const int n = t->notch;
      const std::string notch = n > 0 ? "P" + std::to_string(n) : n == 0 ? "N" : n <= -8 ? "EB" : "B" + std::to_string(-n);
      const int kmh = static_cast<int>(t->v * 3.6 + 0.5);
      ui_.panel({30, 870, 560, 160}, Color{0, 0, 0, 150});
      ui_.text(std::to_string(kmh), 54, 882, 72, theme::kText);
      ui_.text("km/h", 64 + ui_.measure(std::to_string(kmh), 72), 924, 28, theme::kMuted);
      ui_.textRight(notch, 566, 884, 44, n < 0 ? theme::kWarn : theme::kGood);
      const double ds = trains_.distToStop(*t);
      const std::string nxt = t->next_stop >= 0 ? trains_.stations()[static_cast<size_t>(t->next_stop)].name : "-";
      ui_.text(i18n_.f("job.train.hud", {{"station", nxt}, {"d", ds < 1e8 ? std::to_string(static_cast<int>(ds)) : std::string("-")}, {"lim", std::to_string(static_cast<int>(t->vmax * 3.6))}}),
               54, 966, 24, theme::kText);
      ui_.text(tr(t->at_station >= 0 ? "job.train.help_doors" : "job.train.help"), 54, 1000, 20, theme::kMuted);
      if (trains_.atsActive(*t)) ui_.textCentered("ATS", vw / 2, 140, 48, theme::kWarn);
    }
    if (train_msg_t_ > 0) ui_.textCentered(train_msg_, vw / 2, 200, 34, theme::kGood);
  }
  // fishing: tension gauge
  if (fish_.stage == 3) {
    const float x = vw / 2 - 250, y = 700;
    ui_.panel({x - 10, y - 10, 520, 90}, Color{0, 0, 0, 160});
    ui_.text(tr("fish.tension"), x, y - 2, 22, theme::kMuted);
    DrawRectangleRec(ui_.px({x, y + 28, 500, 18}), Color{50, 50, 56, 255});
    DrawRectangleRec(ui_.px({x + 500 * 0.75f, y + 28, 500 * 0.25f, 18}), Color{150, 40, 40, 255});
    DrawRectangleRec(ui_.px({x, y + 28, 500 * static_cast<float>(std::min(1.0, fish_.tension)), 18}), fish_.tension > 0.75 ? theme::kWarn : theme::kGood);
    char b[48];
    std::snprintf(b, sizeof b, "%.0f m", std::max(0.0, fish_.dist));
    ui_.textRight(b, x + 500, y - 2, 22, theme::kText);
  }
  // shop till minigame (modal)
  if (till_.on) {
    const float w = 980, h = 640, x = (vw - w) / 2, y = 150;
    ui_.panel({x, y, w, h}, Color{16, 18, 26, 235});
    ui_.text(tr("till.title"), x + 24, y + 16, 30, theme::kText);
    ui_.textRight(i18n_.f("till.status", {{"n", std::to_string(till_.served)}, {"pay", yen(till_.pay)}, {"m", std::to_string(till_.mistakes)}}), x + w - 24, y + 22, 22,
                  theme::kMuted);
    int64_t total = 0;
    bool need_age = false;
    for (size_t i = 0; i < till_.items.size(); ++i) {
      total += kTill[till_.items[i]].price;
      need_age |= kTill[till_.items[i]].age;
    }
    float yy = y + 70;
    if (till_.stage == 0) {
      ui_.text(tr("till.scan_hint"), x + 24, yy, 22, theme::kMuted);
      yy += 40;
      for (size_t i = 0; i < till_.items.size(); ++i) {
        const TillItem& it = kTill[till_.items[i]];
        const Rectangle r{x + 24 + static_cast<float>(i % 3) * 310, yy + static_cast<float>(i / 3) * 110, 290, 90};
        const bool done = till_.scanned[i];
        DrawRectangleRounded(ui_.px(r), 0.15f, 6, done ? Color{40, 90, 60, 255} : Color{60, 64, 80, 255});
        ui_.text(tr(it.key), r.x + 14, r.y + 12, 24, WHITE);
        ui_.text(done ? "¥" + yen(it.price) : tr("till.unscanned"), r.x + 14, r.y + 50, 22, done ? theme::kGood : theme::kMuted);
        if (!done && ui_.hovered(r) && ui_.clicked()) {
          ui_.consumeClick();
          till_.scanned[i] = true;
        }
      }
      yy += 240;
      int64_t sub = 0;
      bool all = true;
      for (size_t i = 0; i < till_.items.size(); ++i) {
        if (till_.scanned[i]) sub += kTill[till_.items[i]].price;
        all &= till_.scanned[i];
      }
      ui_.text(i18n_.f("till.total", {{"t", yen(sub)}}), x + 24, yy, 34, theme::kText);
      if (need_age && ui_.button({x + w - 560, yy - 6, 250, 56}, tr(till_.age_checked ? "till.age_done" : "till.age"), !till_.age_checked, 24)) till_.age_checked = true;
      if (ui_.button({x + w - 290, yy - 6, 266, 56}, tr("till.to_pay"), all, 26)) {
        if (need_age && !till_.age_checked) {
          ++till_.mistakes;
          till_.msg = tr("till.forgot_age");
          till_.msg_t = 3;
          till_.age_checked = true;
        }
        till_.stage = 1;
      }
    } else {
      ui_.text(i18n_.f("till.total", {{"t", yen(total)}}), x + 24, yy, 34, theme::kText);
      ui_.text(i18n_.f("till.tendered", {{"t", yen(till_.tendered)}}), x + 420, yy, 34, theme::kWarn);
      yy += 60;
      ui_.text(i18n_.f("till.change", {{"t", yen(till_.entered)}}), x + 24, yy, 40, theme::kText);
      yy += 70;
      const int coins[] = {5000, 1000, 500, 100, 50, 10, 5, 1};
      for (int i = 0; i < 8; ++i) {
        const Rectangle r{x + 24 + static_cast<float>(i % 4) * 236, yy + static_cast<float>(i / 4) * 80, 220, 64};
        if (ui_.button(r, "+" + yen(coins[i]), true, 26)) till_.entered += coins[i];
      }
      yy += 180;
      if (ui_.button({x + 24, yy, 220, 60}, tr("till.clear"), true, 26)) till_.entered = 0;
      if (ui_.button({x + w - 300, yy, 276, 60}, tr("till.hand"), true, 28)) {
        if (till_.entered == till_.tendered - total) {
          ++till_.served;
          const int64_t pay = kTillHourly * 3 / 60;  // each customer ~3 minutes of the shift
          till_.pay += pay;
          clock_.advanceGame(180.0);
          if (ledger_) ledger_->transfer(ledger_->externalAccount(), player_account_, pay, rj::econ::TxCategory::Wage, clock_.unixUtc(), tr("till.title"));
          till_.msg = tr("till.thanks");
          till_.msg_t = 2;
          tillNextCustomer();
        } else {
          ++till_.mistakes;
          till_.msg = tr("till.wrong_change");
          till_.msg_t = 3;
          till_.entered = 0;
        }
      }
    }
    if (till_.msg_t > 0) ui_.textCentered(till_.msg, x + w / 2, y + h - 110, 28, theme::kWarn);
    if (ui_.button({x + w / 2 - 150, y + h - 70, 300, 56}, tr("till.finish"), true, 26)) {
      till_.on = false;
      toast(i18n_.f("till.done", {{"n", std::to_string(till_.served)}, {"pay", yen(till_.pay)}}));
    }
    ui_.textCentered(tr("till.note"), vw / 2, y + h + 12, 20, theme::kMuted);
  }
  // photo mode viewfinder
  if (photo_mode_) {
    const float W = vw, H = 1080;
    const Color c{255, 255, 255, 170};
    for (int i = 0; i < 4; ++i) {
      const float x = i % 2 ? W - 140 : 100, y = i / 2 ? H - 140 : 100;
      DrawRectangleRec(ui_.px({x, y, 40, 3}), c);
      DrawRectangleRec(ui_.px({i % 2 ? x + 37 : x, y + (i / 2 ? -37.0f : 0.0f), 3, 40}), c);
    }
    ui_.textCentered(i18n_.f("photo.help", {{"n", std::to_string(photo_spots_.size())}}), W / 2, H - 60, 22, c);
  }
}

// ------------------------------------------------------------------------------------------------
void App::drawPhoneWork(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.work"), cx, yy, 32, theme::kText);
  yy += 46;
  std::string detail;  // long description of the row under the mouse
  auto row = [&](const std::string& title, const std::string& sub, const std::string& btn, bool enabled, const std::string& long_desc) {
    const Rectangle r{cx, yy, cw, 66};
    if (ui_.hovered(r)) detail = long_desc;
    ui_.text(title, cx, yy, 24, theme::kText);
    ui_.text(sub, cx, yy + 32, 16, theme::kMuted);
    const bool hit = ui_.button({cx + cw - 118, yy + 6, 118, 46}, btn, enabled, 20);
    yy += 72;
    return hit;
  };
  const bool taxi_on = jobs_.kind() == JobKind::Taxi, del_on = jobs_.kind() == JobKind::Delivery;
  if (row(tr("job.taxi.name"), tr("job.taxi.short"), tr(taxi_on ? "job.stop" : "job.start"), true, tr("job.taxi.desc"))) {
    if (taxi_on) jobs_.stop();
    else if (!(driving_.active() && driving_.car().type == VehicleType::Taxi)) toast(tr("job.taxi.need_taxi"));
    else if (jobs_.startTaxi(world_, traffic_, player_.pos)) {
      toast(tr("job.taxi.started"));
      screen_ = Screen::Game;
    } else toast(tr("job.none_found"));
  }
  if (row(tr("job.delivery.name"), tr("job.delivery.short"), tr(del_on ? "job.stop" : "job.start"), true, tr("job.delivery.desc"))) {
    if (del_on) jobs_.stop();
    else if (jobs_.startDelivery(world_, traffic_, player_.pos)) {
      toast(tr("job.delivery.started"));
      screen_ = Screen::Game;
    } else toast(tr("job.none_found"));
  }
  if (row(tr("job.train.name"), tr("job.train.short"), tr(drive_train_ >= 0 ? "job.running" : "job.start"), drive_train_ < 0, tr("job.train.desc")))
    startTrainDriving();
  if (row(tr("till.title"), tr("till.short"), tr("job.start"), !till_.on, tr("till.desc"))) {
    till_ = Till{};
    till_.on = true;
    till_.rng ^= static_cast<uint32_t>(clock_.unixUtc()) | 1u;
    tillNextCustomer();
    screen_ = Screen::Game;
  }
  // other occupations: simple shifts (time passes, pay by the game's income table)
  ui_.text(tr("job.shift.title"), cx, yy + 4, 22, theme::kWarn);
  yy += 32;
  ui_.text(tr("job.shift.short"), cx, yy, 16, theme::kMuted);
  yy += 26;
  if (!detail.empty()) {  // hovered job: the full explanation instead of the list
    ui_.panel({cx - 6, yy - 4, cw + 12, bottom - yy}, Color{10, 12, 18, 240});
    ui_.textWrapped(detail, cx, yy + 6, cw, 18, theme::kText);
    return;
  }
  const auto occ = rj::sim::allOccupations();
  const float list_h = bottom - yy - 10;
  const int rows = std::max(1, static_cast<int>(list_h / 50));
  const float wheel = GetMouseWheelMove();
  phone_scroll_ = std::clamp(phone_scroll_ - wheel * 2.0f, 0.0f, static_cast<float>(std::max<int>(0, static_cast<int>(occ.size()) - rows)));
  const int first = static_cast<int>(phone_scroll_);
  for (int i = first; i < static_cast<int>(occ.size()) && i < first + rows; ++i) {
    const auto& o = occ[static_cast<size_t>(i)];
    const int64_t hourly = o.monthly_income_jpy / (21 * 8);
    const std::string name = i18n_.code() == "ja" ? std::string(o.name_ja) : std::string(o.name_en);
    ui_.text(name, cx, yy + 4, 20, theme::kText);
    ui_.text(i18n_.f("job.shift.hourly", {{"y", yen(hourly)}}), cx, yy + 28, 14, theme::kMuted);
    if (ui_.button({cx + cw - 118, yy + 4, 118, 40}, tr("job.shift.work"), true, 17)) {
      const int hours = 4;
      const int64_t pay = hourly * hours;
      clock_.advanceGame(hours * 3600.0);
      if (ledger_) ledger_->transfer(ledger_->externalAccount(), player_account_, pay, rj::econ::TxCategory::Wage, clock_.unixUtc(), name);
      toast(i18n_.f("job.shift.done", {{"job", name}, {"h", std::to_string(hours)}, {"pay", yen(pay)}}));
    }
    yy += 50;
  }
}

void App::drawPhoneHobby(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.hobby"), cx, yy, 32, theme::kText);
  yy += 46;
  std::string detail;
  auto head = [&](const std::string& title, const std::string& sub, const std::string& long_desc) {
    const Rectangle r{cx, yy, cw, 62};
    if (ui_.hovered(r)) detail = long_desc;
    ui_.text(title, cx, yy, 24, theme::kText);
    ui_.text(sub, cx, yy + 32, 16, theme::kMuted);
  };
  // photography
  head(tr("photo.title"), i18n_.f("photo.short", {{"n", std::to_string(photo_spots_.size())}, {"p", std::to_string(photos_taken_)}}),
       i18n_.f("photo.desc", {{"n", std::to_string(photo_spots_.size())}, {"p", std::to_string(photos_taken_)}}));
  if (ui_.button({cx + cw - 118, yy + 6, 118, 46}, tr("photo.open"), true, 20)) {
    photo_mode_ = true;
    photo_fov_ = settings_.fov;
    screen_ = Screen::Game;
  }
  yy += 72;
  // fishing log
  head(tr("fish.title"), i18n_.f("fish.short", {{"n", std::to_string(fish_count_)}}), tr("fish.desc"));
  yy += 62;
  int shown = 0;
  for (const auto& [k, cm] : fish_log_) {
    char b[24];
    std::snprintf(b, sizeof b, "%.1f cm", cm);
    ui_.text(tr(k), cx + 10, yy, 18, theme::kText);
    ui_.textRight(b, cx + cw - 10, yy, 18, theme::kMuted);
    yy += 24;
    if (++shown >= 5) break;
  }
  if (fish_log_.empty()) {
    ui_.text(tr("fish.none"), cx + 10, yy, 18, theme::kMuted);
    yy += 24;
  }
  yy += 14;
  // shrine / temple
  head(tr("worship.title"), i18n_.f("worship.desc", {{"n", std::to_string(worship_count_)}, {"g", std::to_string(goshuin_.size())},
                                                      {"o", last_omikuji_.empty() ? std::string("-") : tr(last_omikuji_)}}),
       tr("worship.long"));
  yy += 72;
  // walking / running
  char km[32];
  std::snprintf(km, sizeof km, "%.2f", player_.distance_walked / 1000.0);
  head(tr("walk.title"), i18n_.f("walk.desc", {{"km", km}}), tr("walk.long"));
  yy += 72;
  if (!detail.empty()) {
    const float top = std::min(yy, bottom - 180);
    ui_.panel({cx - 6, top, cw + 12, bottom - top}, Color{10, 12, 18, 240});
    ui_.textWrapped(detail, cx, top + 10, cw, 18, theme::kText);
  }
}

}  // namespace rjc
