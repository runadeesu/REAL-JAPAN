// The sound scene heard at the camera, built each frame from the simulation, and sound events
// (departure melody, door chimes, ship's horn, cabin chime ...) detected from state changes.
// The audio engine itself (audio/audio.cpp) synthesises everything; levels are game tuning.
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "game/station_names.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

using V3 = rj::geo::Vec3d;
constexpr double kSoundSpeed = 343.0;

struct Ear {
  V3 pos;
  double fx = 0, fy = 1;  // horizontal forward
  V3 vel;
};

float panOf(const Ear& e, const V3& p) {
  const double dx = p.x - e.pos.x, dy = p.y - e.pos.y, d = std::hypot(dx, dy);
  if (d < 0.5) return 0.0f;
  return static_cast<float>(std::clamp((dx * e.fy - dy * e.fx) / d, -1.0, 1.0));  // right of forward = +
}

double dist3(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// Doppler factor for a source moving with `vs` heard by a listener moving with `vl`.
float doppler(const Ear& e, const V3& p, const V3& vs) {
  const double dx = p.x - e.pos.x, dy = p.y - e.pos.y, dz = p.z - e.pos.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d < 0.5) return 1.0f;
  const double rate = (dx * (vs.x - e.vel.x) + dy * (vs.y - e.vel.y) + dz * (vs.z - e.vel.z)) / d;  // d(distance)/dt
  return static_cast<float>(std::clamp(kSoundSpeed / (kSoundSpeed + rate), 0.75, 1.3));
}

void engineOf(VehicleType t, int& cyl, bool& diesel) {
  switch (t) {
    case VehicleType::Kei: cyl = 3; diesel = false; break;
    case VehicleType::Van: cyl = 4; diesel = true; break;
    case VehicleType::Truck:
    case VehicleType::Bus: cyl = 6; diesel = true; break;
    default: cyl = 4; diesel = false; break;
  }
}

// Stable voice slots: keep a key in its slot while it stays among the chosen ones.
template <size_t N>
void assignSlots(int (&slots)[N], const std::vector<int>& keys, int (&out)[N]) {
  for (size_t i = 0; i < N; ++i) out[i] = -1;
  std::vector<int> left;
  for (int k : keys) {
    bool kept = false;
    for (size_t i = 0; i < N; ++i)
      if (slots[i] == k) {
        out[i] = k;
        kept = true;
      }
    if (!kept) left.push_back(k);
  }
  for (int k : left)
    for (size_t i = 0; i < N; ++i)
      if (out[i] < 0) {
        out[i] = k;
        break;
      }
  for (size_t i = 0; i < N; ++i) slots[i] = out[i];
}

}  // namespace

void App::updateDepartureBoards() {
  // Next train on each platform near the camera: its type and destination, how soon (in real
  // minutes: the trains run in real time while the game clock is compressed, so no clock times)
  // and a notice line. Headway operation (no timetable) - the estimate assumes ~13 m/s average.
  const rj::geo::Vec3d cam = rlToEnu(listen_cam_.position);
  const auto& sts = trains_.stations();
  for (size_t si = 0; si < sts.size(); ++si) {
    const Station& st = sts[si];
    if (std::hypot(st.pos.x - cam.x, st.pos.y - cam.y) > 250.0) continue;
    const auto& L = trains_.lines()[static_cast<size_t>(st.line)];
    for (int side : {-1, 1}) {
      const int dir = side < 0 ? 1 : -1;  // trains keep left: dir +1 uses the platform left of the line
      const Train* best = nullptr;
      double best_d = 1e30;
      for (const auto& t : trains_.trains()) {
        if (t.line != st.line || t.dir != dir || t.offmap) continue;
        double d = t.at_station == static_cast<int>(si) ? 0.0 : (st.s - t.s) * dir;
        if (L.closed) {
          d = std::fmod(d, L.length);
          if (d < 0) d += L.length;
        } else if (d < 0) {
          continue;
        }
        if (d < best_d) {
          best_d = d;
          best = &t;
        }
      }
      std::string type, dest, when, notice;
      if (best) {
        type = tr(L.kind == LineKind::Shinkansen ? "board.shinkansen" : "board.local");
        dest = trains_.destination(*best);
        if (dest == "loop+") dest = tr("rail.dest.outer");
        else if (dest == "loop-") dest = tr("rail.dest.inner");
        else if (dest == "mainland") dest = tr("rail.dest.mainland");
        else dest = stationBaseName(dest);
        if (best->at_station == static_cast<int>(si)) {
          when = tr("board.here");
          notice = best->dwell < 6.0 ? tr("board.closing") : (best->dwell < 20.0 ? tr("board.departing") : std::string());
        } else {
          const double eta = best_d / 13.0;
          when = i18n_.f("board.in_min", {{"n", std::to_string(std::max(1, static_cast<int>(std::ceil(eta / 60.0))))}});
          notice = best_d < 500.0 ? tr("board.arriving") : std::string();
        }
      }
      renderer_.setDepartureBoard(static_cast<int>(si), side, type, dest, when, notice, ui_.font());
    }
  }
}

void App::updateSound(float dt) {
  if (!audio_.active()) return;
  dt = std::clamp(dt, 0.0f, 0.25f);
  if (ui_.takePresses() > 0) audio_.cue(Cue::Click, 0.5f);  // menu and phone buttons
  SoundScene sc;
  sc.master = settings_.volume / 100.0f;
  if (!listen_game_) {  // title screen: the city far below
    sc.traffic = 0.25f;
    sc.outdoor = 0.6f;
    sc.daylight = 0.0f;
    audio_.setScene(sc);
    audio_.advanceOffline(dt);
    return;
  }
  if (screen_ != Screen::Game && screen_ != Screen::Phone) sc.master *= 0.35f;  // paused / menus

  Ear ear;
  ear.pos = rlToEnu(listen_cam_.position);
  {
    const V3 t = rlToEnu(listen_cam_.target);
    const double fx = t.x - ear.pos.x, fy = t.y - ear.pos.y, fl = std::hypot(fx, fy);
    if (fl > 1e-6) {
      ear.fx = fx / fl;
      ear.fy = fy / fl;
    }
  }
  if (snd_ear_ok_ && dt > 1e-4f) {
    const V3 v{(ear.pos.x - snd_ear_prev_.x) / dt, (ear.pos.y - snd_ear_prev_.y) / dt, (ear.pos.z - snd_ear_prev_.z) / dt};
    if (std::hypot(v.x, v.y) < 400.0) ear.vel = v;  // (not across a teleport / origin rebase)
  }
  snd_ear_prev_ = ear.pos;
  snd_ear_ok_ = true;

  // --- where the listener is: open air, a vehicle cabin, underground -------------------------
  const bool on_foot = ride_train_ < 0 && ride_jet_ < 0 && ride_ferry_ < 0 && !flying_ && !driving_.active();
  float outdoor = 1.0f, reverb = 0.05f, room = 0.35f;
  if (ride_train_ >= 0) outdoor = 0.12f, reverb = 0.02f;
  else if (ride_jet_ >= 0) outdoor = 0.0f, reverb = 0.0f;
  else if (flying_) outdoor = fly_cockpit_ ? 0.1f : 0.7f, reverb = 0.0f;
  else if (driving_.active()) outdoor = drive_first_person_ ? 0.2f : 0.85f, reverb = drive_first_person_ ? 0.0f : 0.04f;
  if (underground_ > 0.01f || !inside_id_.empty()) {
    const float u = inside_id_.empty() ? underground_ : 1.0f;
    outdoor = std::min(outdoor, 1.0f - 0.85f * u);
    reverb = 0.05f + 0.3f * u;
    room = 0.8f;
  }
  int platform_station = -1;
  if (on_foot && trains_.loaded()) {
    const int si = trains_.stationNear(player_.pos, 190.0);
    if (si >= 0 && std::fabs(player_.pos.z - trains_.stations()[static_cast<size_t>(si)].pos.z) < 2.2) {
      platform_station = si;
      reverb = std::max(reverb, 0.12f);  // under the platform canopy
      room = std::max(room, 0.5f);
    }
  }
  const float muffle = std::clamp(1.0f - outdoor, 0.0f, 0.85f);
  sc.outdoor = outdoor;
  sc.reverb = reverb;
  sc.room = room;
  sc.daylight = 1.0f - lighting_.night;
  sc.rain = lighting_.rain;
  sc.wind = std::clamp(lighting_.wind * 0.6f, 0.0f, 1.0f);

  // --- road traffic: distant rumble + the nearest cars as voices -----------------------------
  {
    struct Near {
      const Vehicle* v;
      double d;
    };
    std::vector<Near> nearv;
    double w = 0;
    for (const auto& v : traffic_.vehicles()) {
      const double d = dist3(v.pos, ear.pos);
      if (d > 260.0) continue;
      w += (0.3 + std::min(std::fabs(v.v), 15.0) / 15.0) / (1.0 + d / 30.0);
      if (d < 90.0) nearv.push_back({&v, d});
    }
    sc.traffic = static_cast<float>(std::min(1.0, w / 5.0));
    std::sort(nearv.begin(), nearv.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    if (nearv.size() > 4) nearv.resize(4);
    std::vector<int> keys;
    for (const auto& n : nearv) keys.push_back(n.v->id);
    int slots[4];
    assignSlots(snd_car_keys_, keys, slots);
    for (int i = 0; i < 4; ++i) {
      if (slots[i] < 0) continue;
      const Near* n = nullptr;
      for (const auto& q : nearv)
        if (q.v->id == slots[i]) n = &q;
      if (!n) continue;
      const Vehicle& v = *n->v;
      CarSound& c = sc.cars[i + 1];
      c.on = true;
      c.key = v.id;
      engineOf(v.type, c.cylinders, c.diesel);
      const double spd = std::fabs(v.v);
      c.rpm = static_cast<float>((c.diesel ? 650.0 : 750.0) + std::min(spd, 20.0) * (c.diesel ? 60.0 : 85.0) + (v.acc > 0.3 ? 500.0 : 0.0));
      c.load = static_cast<float>(std::clamp(0.2 + v.acc * 0.4, 0.05, 1.0));
      c.speed = static_cast<float>(spd);
      const bool big = v.type == VehicleType::Bus || v.type == VehicleType::Truck;
      c.gain = static_cast<float>((big ? 0.95 : 0.7) / (1.0 + n->d / 7.0));
      c.pan = panOf(ear, v.pos);
      const V3 vs{std::sin(v.yaw) * v.v, std::cos(v.yaw) * v.v, 0.0};
      c.doppler = doppler(ear, v.pos, vs);
      c.muffle = muffle;
    }
  }

  // --- the player's car -----------------------------------------------------------------
  if (driving_.active()) {
    CarSound& c = sc.cars[0];
    c.on = true;
    c.key = -100 - driving_.car().id;
    engineOf(driving_.car().type, c.cylinders, c.diesel);
    c.rpm = driving_.rpm();
    c.load = driving_.throttle();
    c.speed = static_cast<float>(std::fabs(driving_.car().v));
    c.slip = driving_.slip();
    c.gain = 0.9f;
    c.muffle = drive_first_person_ ? 0.55f : 0.12f;
    if (const float imp = driving_.takeImpact(); imp > 1.5f) audio_.cue(Cue::Crash, std::min(1.0f, imp / 12.0f));
    snd_horn_t_ -= dt;
    if (screen_ == Screen::Game && IsKeyDown(KEY_H) && snd_horn_t_ <= 0.0f) {
      audio_.cue(Cue::Horn, drive_first_person_ ? 0.5f : 0.7f);
      snd_horn_t_ = 0.38f;
    }
  }

  // --- footsteps, pedestrian signals -----------------------------------------------------
  if (on_foot && !player_.fly) {
    const double spd = ear.vel.x == 0 && ear.vel.y == 0 ? 0.0 : std::hypot(ear.vel.x, ear.vel.y);
    if (spd > 0.4 && spd < 9.0 && player_.grounded) sc.step_rate = static_cast<float>(spd / (spd > 2.6 ? 1.15 : 0.72));
    // guide tones at crossings with an acoustic device (assumed at about half of them), which
    // (as commonly in Japan) sound only in the daytime, while the pedestrian light is green
    const int hour = jst().hour;
    if (hour >= 8 && hour < 19) {
      double best = 22.0;
      const TrafficSignals::Head* hb = nullptr;
      for (const auto& h : signals_.heads()) {
        if (h.kind != 1) continue;
        const double d = std::hypot(h.pos.x - ear.pos.x, h.pos.y - ear.pos.y);
        if (d >= best || ((static_cast<uint32_t>(h.group) * 2654435761u) >> 28) % 10 >= 5) continue;
        if (signals_.pedestrian(h.group, h.phase) != PedLamp::Walk) continue;
        best = d;
        hb = &h;
      }
      if (hb) {
        sc.crossing = std::fabs(std::sin(hb->facing)) > 0.707 ? 1 : 2;
        sc.crossing_gain = static_cast<float>(0.9 / (1.0 + best / 6.0));
        sc.crossing_pan = panOf(ear, hb->pos);
      }
    }
    // the sea against the quay when standing at the water
    if (frame_ % 10 == 0) {
      double sz;
      V3 spot;
      snd_sea_ = waterAhead(sz, spot) ? 0.3f : 0.0f;
    }
    sc.sea = snd_sea_;
  }

  // --- trains --------------------------------------------------------------------------
  if (trains_.loaded()) {
    auto trainAcc = [&](const Train& t) {
      auto it = snd_train_v_.find(t.id);
      float& a = snd_train_acc_[t.id];
      if (it != snd_train_v_.end() && dt > 1e-4f) a += (static_cast<float>((t.v - it->second) / dt) - a) * std::min(1.0f, dt * 4.0f);
      snd_train_v_[t.id] = t.v;
      return a;
    };
    auto trainCurve = [&](const Train& t, int car) {  // |d heading / ds| between neighbouring cars
      const int k0 = std::max(0, car - 1), k1 = std::min(t.cars - 1, car + 1);
      if (k1 <= k0) return 0.0f;
      rj::geo::Vec3d p0, p1;
      float y0, y1, pt;
      trains_.carPose(t, k0, p0, y0, pt);
      trains_.carPose(t, k1, p1, y1, pt);
      const double ds = std::max(1.0, std::hypot(p1.x - p0.x, p1.y - p0.y));
      return static_cast<float>(std::fabs(std::remainder(static_cast<double>(y1 - y0), 2.0 * PI)) / ds);
    };
    auto traction = [&](const Train& t, float acc) {
      if (t.manual) return t.notch > 0 ? t.notch / 5.0f : (t.notch < 0 && t.v > 0.3 ? std::max(-1.0f, t.notch / 7.0f) : 0.0f);
      if (t.at_station >= 0 || t.v < 0.05) return 0.0f;
      return acc > 0.05f ? std::min(1.0f, acc / 0.7f) : (acc < -0.1f ? std::max(-1.0f, acc / 0.8f) : 0.0f);
    };
    // the train ridden
    if (const Train* t = ride_train_ >= 0 ? trains_.train(ride_train_) : nullptr) {
      const bool shink = trains_.lines()[static_cast<size_t>(t->line)].kind == LineKind::Shinkansen;
      RailSound& r = sc.rail[0];
      r.on = true;
      r.key = t->id;
      r.shinkansen = shink;
      r.s = t->s;
      r.v = static_cast<float>(t->v);
      r.dir = t->dir;
      r.cars = std::min(t->cars, 16);
      r.car_len = static_cast<float>(t->car_len);
      r.traction = traction(*t, trainAcc(*t));
      for (int k = 0; k < r.cars; ++k) {
        const float dk = static_cast<float>((k - ride_car_) * t->car_len) / 9.0f;
        r.car_gain[k] = 1.0f / (1.0f + dk * dk);
      }
      r.motor_gain = shink ? 0.35f : 0.7f;
      r.air_gain = shink ? 0.6f : 0.12f;
      r.curve = trainCurve(*t, ride_car_);
      r.muffle = drive_train_ >= 0 ? 0.4f : 0.5f;
    }
    // other trains nearby (platform, street under the viaduct)
    struct NearT {
      const Train* t;
      double d;
    };
    std::vector<NearT> nt;
    for (const auto& t : trains_.trains()) {
      if (t.id == ride_train_ || t.offmap) continue;
      rj::geo::Vec3d p;
      float yaw, pitch;
      trains_.carPose(t, t.cars / 2, p, yaw, pitch);
      const double d = dist3(p, ear.pos) - t.cars * t.car_len * 0.5;
      if (d < 250.0) nt.push_back({&t, std::max(0.0, d)});
    }
    std::sort(nt.begin(), nt.end(), [](const NearT& a, const NearT& b) { return a.d < b.d; });
    if (nt.size() > 2) nt.resize(2);
    std::vector<int> keys;
    for (const auto& n : nt) keys.push_back(n.t->id);
    int slots[2];
    assignSlots(snd_rail_keys_, keys, slots);
    for (int i = 0; i < 2; ++i) {
      const NearT* n = nullptr;
      for (const auto& q : nt)
        if (q.t->id == slots[i]) n = &q;
      if (!n) continue;
      const Train& t = *n->t;
      const bool shink = trains_.lines()[static_cast<size_t>(t.line)].kind == LineKind::Shinkansen;
      RailSound& r = sc.rail[i + 1];
      r.on = true;
      r.key = t.id;
      r.shinkansen = shink;
      r.s = t.s;
      r.v = static_cast<float>(t.v);
      r.dir = t.dir;
      r.cars = std::min(t.cars, 16);
      r.car_len = static_cast<float>(t.car_len);
      r.traction = traction(t, trainAcc(t));
      double dmin = 1e9;
      V3 pmin{};
      for (int k = 0; k < r.cars; ++k) {
        rj::geo::Vec3d p;
        float yaw, pitch;
        trains_.carPose(t, k, p, yaw, pitch);
        const double d = dist3(p, ear.pos);
        if (d < dmin) {
          dmin = d;
          pmin = p;
        }
        const float q = static_cast<float>(d / 15.0);
        r.car_gain[k] = 1.2f / (1.0f + q * q);
      }
      const float q = static_cast<float>(dmin / 20.0);
      r.motor_gain = 0.6f / (1.0f + q * q);
      r.air_gain = shink ? static_cast<float>(1.0 / (1.0 + dmin / 40.0)) : 0.0f;
      r.pan = panOf(ear, pmin);
      r.muffle = muffle;
      r.curve = trainCurve(t, t.cars / 2);
    }
    // station sounds: departure melody, door chime, door engines; the chime on board
    const int here = ride_train_ >= 0 ? (trains_.train(ride_train_) ? trains_.train(ride_train_)->at_station : -1) : platform_station;
    for (const auto& t : trains_.trains()) {
      const int prev_at = snd_at_.count(t.id) ? snd_at_[t.id] : -1;
      const bool aboard = t.id == ride_train_;
      if (t.at_station >= 0 && t.at_station == here && !t.manual) {
        const double prev = snd_dwell_.count(t.id) ? snd_dwell_[t.id] : 1e9;
        rj::geo::Vec3d p;
        float yaw, pitch;
        trains_.carPose(t, t.cars / 2, p, yaw, pitch);
        const float pn = aboard ? 0.0f : panOf(ear, p) * 0.5f;
        if (prev > 15.0 && t.dwell <= 15.0 && t.dwell > 6.0) {
          audio_.cue(Cue::DepartureMelody, aboard ? 0.25f : 0.5f, pn);
          std::string dest = trains_.destination(t);
          dest = dest == "loop+" ? tr("rail.dest.outer") : dest == "loop-" ? tr("rail.dest.inner") : stationBaseName(dest);
          caption(i18n_.f("pa.departing", {{"dest", dest}}));
        }
        if (prev > 4.0 && t.dwell <= 4.0) audio_.cue(Cue::DoorChime, aboard ? 0.55f : 0.4f, pn);
        if (prev > 1.3 && t.dwell <= 1.3) audio_.cue(Cue::DoorAir, aboard ? 0.6f : 0.3f, pn);
      }
      if (aboard && prev_at < 0 && t.at_station >= 0) {  // doors open on arrival
        audio_.cue(Cue::DoorAir, 0.6f);
        caption(i18n_.f("pa.arrived", {{"st", trains_.stations()[static_cast<size_t>(t.at_station)].name}}));
      }
      if (aboard && prev_at >= 0 && t.at_station < 0) {
        if (t.manual) audio_.cue(Cue::DoorAir, 0.6f);
        snd_chime_t_ = 7.0f;  // next-stop announcement shortly after leaving
      }
      snd_dwell_[t.id] = t.at_station >= 0 ? t.dwell : 1e9;
      snd_at_[t.id] = t.at_station;
    }
    if (snd_chime_t_ > 0.0f && (snd_chime_t_ -= dt) <= 0.0f && ride_train_ >= 0) {
      audio_.cue(Cue::TrainChime, 0.45f);
      if (const Train* rt = trains_.train(ride_train_); rt && rt->next_stop >= 0)
        caption(i18n_.f("pa.next", {{"st", trains_.stations()[static_cast<size_t>(rt->next_stop)].name}}));
    }
    // level crossings: the bell rings twice a second while the lamps flash (the nearest one heard)
    snd_bell_t_ -= dt;
    if (snd_bell_t_ <= 0.0f) {
      snd_bell_t_ = 0.5f;
      double best = 260.0;
      const LevelCrossing* near = nullptr;
      for (const auto& c : trains_.crossings())
        if (c.warning)
          if (const double d = dist3(c.pos, ear.pos); d < best) best = d, near = &c;
      if (near) {
        const float g = static_cast<float>(std::clamp(9.0 / std::max(best, 9.0), 0.0, 1.0)) * (ride_train_ >= 0 ? 0.35f : 0.8f);
        audio_.cue(Cue::CrossingBell, g, panOf(ear, near->pos) * 0.6f);
      }
    }
  }

  // --- ferries -------------------------------------------------------------------------
  for (const auto& f : ferries_.ships()) {
    const int ph = static_cast<int>(f.phase);
    auto it = snd_ferry_phase_.find(f.id);
    const double d = dist3(f.pos, ear.pos);
    if (it != snd_ferry_phase_.end() && it->second == static_cast<int>(Ferry::Phase::Docked) && f.phase == Ferry::Phase::Astern && d < 5000.0)
      audio_.cue(Cue::ShipHorn, static_cast<float>(std::min(0.9, 70.0 / (d + 70.0))), f.id == ride_ferry_ ? 0.0f : panOf(ear, f.pos));
    snd_ferry_phase_[f.id] = ph;
    const float load = static_cast<float>(std::clamp(std::fabs(f.v) / 7.0, 0.15, 1.0));
    if (f.id == ride_ferry_) {
      sc.ship_gain = 0.45f;
      sc.ship_engine = load;
      sc.sea = static_cast<float>(0.35 + std::min(0.4, std::fabs(f.v) / 15.0));
    } else if (d < 300.0 && f.phase != Ferry::Phase::Offmap) {
      sc.ship_gain = std::max(sc.ship_gain, static_cast<float>(0.5 / (1.0 + d / 40.0)) * (1.0f - muffle));
      sc.ship_engine = std::max(sc.ship_engine, load);
      sc.sea = std::max(sc.sea, static_cast<float>(f.wake * 0.4 / (1.0 + d / 60.0)));
    }
  }

  // --- aircraft ------------------------------------------------------------------------
  auto thrustOf = [](const Airliner& a) {
    switch (a.phase) {
      case Airliner::Phase::AtStand: return 0.05f;
      case Airliner::Phase::Pushback: return 0.15f;
      case Airliner::Phase::TaxiOut:
      case Airliner::Phase::TaxiIn: return 0.25f;
      case Airliner::Phase::Takeoff: return 1.0f;
      case Airliner::Phase::Climb: return 0.85f;
      case Airliner::Phase::Offmap: return 0.7f;
      case Airliner::Phase::Approach: return 0.4f;
      case Airliner::Phase::Landing: return a.v > 35.0 ? 0.8f : 0.3f;  // reverse thrust on the roll-out
    }
    return 0.0f;
  };
  if (const Airliner* a = ride_jet_ >= 0 ? aviation_.airliner(ride_jet_) : nullptr) {
    sc.jet_gain = 0.55f;
    sc.jet = thrustOf(*a);
    const bool ground = a->phase != Airliner::Phase::Climb && a->phase != Airliner::Phase::Offmap && a->phase != Airliner::Phase::Approach;
    sc.jet_rumble = ground ? static_cast<float>(std::min(1.0, a->v / 45.0)) : 0.0f;
    const int ph = static_cast<int>(a->phase);
    if (snd_jet_phase_ >= 0 && ph != snd_jet_phase_ && (a->phase == Airliner::Phase::Pushback || a->phase == Airliner::Phase::Approach)) {
      audio_.cue(Cue::CabinChime, 0.5f);
      caption(a->phase == Airliner::Phase::Pushback ? i18n_.f("pa.jet_depart", {{"ap", airportName(a->to)}}) : tr("pa.jet_approach"));
    }
    if ((snd_jet_gear_ - 0.5f) * (a->gear - 0.5f) < 0.0f) audio_.cue(Cue::GearThunk, 0.6f);
    snd_jet_phase_ = ph;
    snd_jet_gear_ = a->gear;
  } else {
    snd_jet_phase_ = -1;
    double best = 4000.0;
    for (const auto& a : aviation_.airliners()) {
      if (a.phase == Airliner::Phase::Offmap || a.phase == Airliner::Phase::AtStand) continue;
      const double d = dist3(a.pos, ear.pos);
      if (d < best) {
        best = d;
        sc.jet = thrustOf(a);
        sc.jet_gain = static_cast<float>(1.1 / (1.0 + d / 120.0)) * (1.0f - 0.6f * muffle);
      }
    }
  }
  if (flying_) {
    const LightPlane& pl = aviation_.plane();
    sc.prop_rpm = pl.rpm();
    sc.prop_gain = pl.crashed() ? 0.0f : (fly_cockpit_ ? 0.55f : 0.45f);
    sc.airflow = static_cast<float>(std::min(1.5, pl.airspeed() / 55.0));
    sc.stall_horn = !pl.onGround() && !pl.crashed() && pl.alpha() > 0.24;
    if (pl.crashed() && !snd_crashed_) audio_.cue(Cue::Crash, 1.0f);
    snd_crashed_ = pl.crashed();
  } else {
    snd_crashed_ = false;
  }
  sc.music = band_on_ ? band_track_ : music_track_;  // the band's rhythm section, or the phone's music player
  sc.music_gain = band_on_ ? 0.4f : music_track_ >= 0 ? 0.55f : 0.0f;
  sc.music_backing = band_on_;

  audio_.setScene(sc);
  audio_.advanceOffline(dt);
}

}  // namespace rjc
