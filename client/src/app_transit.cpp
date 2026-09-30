// On foot through the stations and in the trains, in first person (no teleporting prompts):
//  * the IC ticket gates open as the player walks through a lane; the card is checked on the way
//    in (the minimum fare must be on it) and the fare for the distance ridden is taken on the way
//    out (game fares, see railFare); the flaps shut in the player's face when the card is short
//  * a stopped train is boarded by walking through one of its open doors from the platform, and
//    left the same way; inside, the player walks about the car (the aisle, the vestibules, the
//    gangways into the next cars), looks at a free seat and sits down (E or click), and stands up
//    again (E, or a movement key)

#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "game/car_layout.hpp"
#include "game/deck_layout.hpp"
#include "raymath.h"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

using V3 = rj::geo::Vec3d;
constexpr float kStandEye = 1.62f;  // eye above the car floor, standing
constexpr float kSeatEye = 1.17f;   // and seated
constexpr float kPiF = 3.14159265358979f;

float wrapPi(float a) { return std::remainder(a, 2.0f * kPiF); }

}  // namespace

bool App::usePressed() const {
  return screen_ == Screen::Game && (IsKeyPressed(KEY_E) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
}

void App::enterCar(int train, int car, float x, float y) {
  ride_train_ = train;
  ride_car_ = car;
  ob_x_ = x;
  ob_y_ = y;
  ob_seat_ = -1;
  ob_sitting_ = false;
  ob_sit_ = 0.0f;
  ob_prev_train_ = -1;
  ob_path_.clear();
}

void App::leaveCar(const V3& w) {
  const Train* t = trains_.train(ride_train_);
  if (t) player_.yaw = carFrame(trains_, *t, ride_car_).yaw + ride_look_yaw_;
  player_.pitch = ride_look_pitch_;
  ride_train_ = -1;
  ob_seat_ = -1;
  ob_sitting_ = false;
  ob_sit_ = 0.0f;
  ob_path_.clear();
  player_.pos = w;
  player_.vel_z = 0;
  player_.fly = false;
  player_.snapToGround(world_);
  player_.cam_z_init = false;
}

// A standing place in car k (the middle of the aisle, halfway along) - after driving, or when a
// saved game puts the player aboard.
void App::standInCar(int k) {
  const Train* t = trains_.train(ride_train_);
  if (!t) return;
  const CarLayout L = carLayoutOf(trains_, *t, k);
  enterCar(t->id, k, L.shink ? -0.28f : 0.0f, (L.y0 + L.y1) * 0.5f);
}

void App::updateOnBoard(float dt) {
  dt = std::min(dt, 0.1f);
  const Train* t = trains_.train(ride_train_);
  if (!t) {
    ride_train_ = -1;
    return;
  }
  const bool input = screen_ == Screen::Game;
  const bool test = ride_test_t_ >= 0.0f && !ride_test_done_;
  if (input) {
    const Vector2 md = GetMouseDelta();
    const float sens = 0.0022f * settings_.mouse_sensitivity;
    ride_look_yaw_ = wrapPi(ride_look_yaw_ + md.x * sens);
    ride_look_pitch_ = std::clamp(ride_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -1.35f, 1.35f);
  }
  CarFrame F = carFrame(trains_, *t, ride_car_);
  CarLayout L = carLayoutOf(trains_, *t, ride_car_);
  const float open = Trains::doorOpen(*t);
  const bool left = doorSideLeft(*t, ride_car_);
  const bool oL = left && open > 0.6f, oR = !left && open > 0.6f;
  // fare: the distance the train carries the player (the IC card pays for it at the exit gate)
  {
    const RailLine& RL = trains_.lines()[static_cast<size_t>(t->line)];
    if (ob_prev_train_ == t->id) {
      double ds = std::fabs(t->s - ob_prev_s_);
      if (RL.closed) ds = std::min(ds, RL.length - ds);
      if (ds < 400.0 && ds > 0.0) {
        paid_km_ += ds / 1000.0;
        if (RL.kind == LineKind::Shinkansen) paid_shink_ = true;
      }
    }
    ob_prev_train_ = t->id;
    ob_prev_s_ = t->s;
  }
  // where the player wants to go (car frame): the keys, relative to the view, or the test's path
  float wx = 0.0f, wy = 0.0f, reach = 1e9f;  // (reach: distance to the test path's next point)
  bool run = false;
  if (input && !test) {
    const float a = ride_look_yaw_;
    const float fwd = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.0f : 0.0f) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.0f : 0.0f);
    const float side = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
    wx = std::sin(a) * fwd + std::cos(a) * side;
    wy = std::cos(a) * fwd - std::sin(a) * side;
    run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  }
  if (!ob_path_.empty() && !ob_sitting_) {  // test aid: walk the path
    const auto [tx, ty] = ob_path_.front();
    const float dx = tx - ob_x_, dy = ty - ob_y_, d = std::hypot(dx, dy);
    if (d < 0.08f) {
      ob_path_.erase(ob_path_.begin());
    } else {
      wx = dx / d;
      wy = dy / d;
      reach = d;
      run = true;
      ride_look_yaw_ = std::atan2(dx, dy);
    }
  }
  const float wl = std::hypot(wx, wy);
  if (wl > 1e-4f) wx /= wl, wy /= wl;
  const bool moving = wl > 1e-4f;
  if (ob_sitting_) {
    ob_sit_ = std::min(1.0f, ob_sit_ + dt * 2.2f);
    // turn to face the way the seat faces while sitting down
    if (ob_sit_ < 1.0f && ob_seat_ >= 0 && ob_seat_ < static_cast<int>(L.seats.size()))
      ride_look_yaw_ += wrapPi(L.seats[static_cast<size_t>(ob_seat_)].facing - ride_look_yaw_) * std::min(1.0f, dt * 5.0f);
    if (ob_sit_ >= 1.0f && moving && input) ob_sitting_ = false;  // a step: stand up
  } else {
    ob_sit_ = std::max(0.0f, ob_sit_ - dt * 2.6f);
    if (ob_sit_ <= 0.0f) {
      ob_seat_ = -1;
      if (moving) {
        const float spd = std::min((run ? 2.6f : 1.3f) * dt, reach);
        const float nx = ob_x_ + wx * spd, ny = ob_y_ + wy * spd;
        if (carWalkable(L, nx, ny, oL, oR)) ob_x_ = nx, ob_y_ = ny;
        else if (carWalkable(L, nx, ob_y_, oL, oR)) ob_x_ = nx;
        else if (carWalkable(L, ob_x_, ny, oL, oR)) ob_y_ = ny;
        ob_bob_ += spd * 3.3f;
      }
      // doors closing on the player in the doorway: back inside
      if (!carWalkable(L, ob_x_, ob_y_, oL, oR) && std::fabs(ob_x_) > L.half_w - 0.3f)
        ob_x_ = std::copysign(L.half_w - 0.3f, ob_x_);
      // out of the open door onto the platform
      if (std::fabs(ob_x_) > L.half_w + 0.3f && (ob_x_ < 0 ? oL : oR)) {
        const V3 w = F.at(ob_x_ + std::copysign(0.3f, ob_x_), ob_y_, L.floor_z);
        TraceLog(LOG_INFO, "RJ: stepped off train %d car %d onto the platform at %.1f %.1f %.1f", t->id, ride_car_, w.x, w.y, w.z);
        leaveCar(w);
        return;
      }
      // through the gangway into the next car
      if (ob_y_ > L.y1 + 0.1f || ob_y_ < L.y0 - 0.1f) {
        const V3 w = F.at(ob_x_, ob_y_, 0.0);
        for (int nk : {ride_car_ - 1, ride_car_ + 1}) {
          if (nk < 0 || nk >= t->cars) continue;
          const CarFrame G = carFrame(trains_, *t, nk);
          const CarLayout M = carLayoutOf(trains_, *t, nk);
          double x, y;
          G.local(w, x, y);
          if (y < M.y0 - 0.7 || y > M.y1 + 0.7 || std::fabs(x) > 0.7) continue;
          ride_look_yaw_ = wrapPi(ride_look_yaw_ + F.yaw - G.yaw);
          ride_car_ = nk;
          ob_x_ = static_cast<float>(x);
          ob_y_ = static_cast<float>(y);
          F = G;
          L = M;
          TraceLog(LOG_INFO, "RJ: walked through the gangway into car %d", nk);
          break;
        }
      }
    }
  }
  // the player's pose in the world (other systems follow it: streaming, sound, the map)
  float px = ob_x_, py = ob_y_;
  if (ob_seat_ >= 0 && ob_seat_ < static_cast<int>(L.seats.size())) {
    const SeatSlot& s = L.seats[static_cast<size_t>(ob_seat_)];
    const float k = ob_sit_ * ob_sit_ * (3.0f - 2.0f * ob_sit_);
    px = ob_x_ + (s.x - ob_x_) * k;
    py = ob_y_ + (s.y - ob_y_) * k;
  }
  player_.pos = F.at(px, py, L.floor_z);
  player_.yaw = F.yaw + ride_look_yaw_;
  player_.pitch = ride_look_pitch_;
  player_.cam_z = player_.pos.z;
  player_.cam_z_init = true;
  player_.bob_amount = 0.0f;
}

Camera3D App::rideCamera() const {
  const Train* t = trains_.train(ride_train_);
  if (!t) return player_.camera(settings_.fov);
  const CarFrame F = carFrame(trains_, *t, ride_car_);
  const CarLayout L = carLayoutOf(trains_, *t, ride_car_);
  float px = ob_x_, py = ob_y_, eh = kStandEye;
  const float k = ob_sit_ * ob_sit_ * (3.0f - 2.0f * ob_sit_);
  if (ob_seat_ >= 0 && ob_seat_ < static_cast<int>(L.seats.size())) {
    const SeatSlot& s = L.seats[static_cast<size_t>(ob_seat_)];
    // seated: the head a little ahead of the hips, over the seat
    const float hx = s.x + std::sin(s.facing) * 0.1f, hy = s.y + std::cos(s.facing) * 0.1f;
    px = ob_x_ + (hx - ob_x_) * k;
    py = ob_y_ + (hy - ob_y_) * k;
    eh = kStandEye + (kSeatEye - kStandEye) * k;
  } else {
    eh += std::sin(ob_bob_ * 2.0f) * 0.015f;  // a step's bob
  }
  const V3 eye = F.at(px, py, L.floor_z + eh);
  const float y = F.yaw + ride_look_yaw_, pt = F.pitch + ride_look_pitch_;
  const V3 f{std::sin(y) * std::cos(pt), std::cos(y) * std::cos(pt), std::sin(pt)};
  Camera3D c{};
  c.position = enuToRl(eye);
  c.target = enuToRl({eye.x + f.x, eye.y + f.y, eye.z + f.z});
  c.up = {0, 1, 0};
  c.fovy = settings_.fov;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

// Seats in reach and in view; E / click sits down, E stands up again.
void App::updateSeatChoice() {
  const Train* t = trains_.train(ride_train_);
  if (!t || drive_train_ >= 0) return;
  const CarLayout L = carLayoutOf(trains_, *t, ride_car_);
  if (ob_sitting_) {
    if (ob_sit_ >= 1.0f) {
      aim_icon_ = AimIcon::Stand;
      aim_label_ = tr("aim.stand");
      if (usePressed()) ob_sitting_ = false;
    }
    return;
  }
  if (ob_sit_ > 0.0f) return;
  // the view ray in the car frame (horizontal direction and pitch)
  const float a = ride_look_yaw_, p = ride_look_pitch_;
  const float dx = std::sin(a) * std::cos(p), dy = std::cos(a) * std::cos(p), dz = std::sin(p);
  int best = -1;
  float best_c = std::cos(13.0f * DEG2RAD);
  for (size_t i = 0; i < L.seats.size(); ++i) {
    const SeatSlot& s = L.seats[i];
    const float ox = s.x - ob_x_, oy = s.y - ob_y_, oz = 0.5f - kStandEye;
    const float d = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (std::hypot(ox, oy) > 1.75f || d < 0.05f) continue;
    const float c = (ox * dx + oy * dy + oz * dz) / d;
    if (c > best_c) {
      best_c = c;
      best = static_cast<int>(i);
    }
  }
  aim_seat_ = best;
  if (best < 0) return;
  const bool taken = crowd_.seatTaken(trains_, t->id, ride_car_, best);
  aim_icon_ = taken ? AimIcon::Blocked : AimIcon::Seat;
  aim_label_ = tr(taken ? "aim.seat_taken" : "aim.sit");
  if (!taken && usePressed()) {
    ob_seat_ = best;
    ob_sitting_ = true;
    ob_sit_ = 0.0f;
    TraceLog(LOG_INFO, "RJ: sat down on seat %d of car %d (train %d)", best, ride_car_, t->id);
  }
}

// On foot on a platform: walking into an open door of a stopped train boards it.
void App::updateBoarding() {
  if (ride_train_ >= 0 || player_.fly || !inside_id_.empty() || driving_.active() || flying_) return;
  const V3 me = player_.pos;
  for (const auto& t : trains_.trains()) {
    if (t.at_station < 0 || Trains::doorOpen(t) < 0.6f) continue;
    const Station& st = trains_.stations()[static_cast<size_t>(t.at_station)];
    if (std::hypot(st.pos.x - me.x, st.pos.y - me.y) > 320.0) continue;
    for (int k = 0; k < t.cars; ++k) {
      const CarFrame F = carFrame(trains_, t, k);
      if (std::hypot(F.p.x - me.x, F.p.y - me.y) > 16.0) continue;
      const CarLayout L = carLayoutOf(trains_, t, k);
      double x, y;
      F.local(me, x, y);
      const double zf = F.p.z + L.floor_z + F.tanp * y;
      if (std::fabs(me.z - zf) > 0.75) continue;
      const bool left = doorSideLeft(t, k);
      const double out = left ? -x : x;  // how far out on the platform side
      if (out < L.half_w - 0.2 || out > L.half_w + 0.75) continue;
      bool door = false;
      for (const auto& d : L.doors)
        if (y > d.first + 0.12 && y < d.second - 0.12) door = true;
      if (!door) continue;
      // stepping in (towards the car)
      const double ix = left ? F.rx : -F.rx, iy = left ? F.ry : -F.ry;
      if (player_.wish.x * ix + player_.wish.y * iy < 0.3) continue;
      enterCar(t.id, k, static_cast<float>((left ? -1.0 : 1.0) * (L.half_w + 0.15)), static_cast<float>(y));
      ride_look_yaw_ = wrapPi(player_.yaw - F.yaw);
      ride_look_pitch_ = player_.pitch;
      TraceLog(LOG_INFO, "RJ: stepped aboard train %d car %d at y %.1f", t.id, k, y);
      return;
    }
  }
}

// Level crossings: a lowered barrier arm stops the player on foot and the player's car; the player
// (or the player's car) on the tracks trips the obstacle detector, which stops the trains.
void App::updateCrossingSafety() {
  crossing_walls_.clear();
  for (const auto& c : trains_.crossings()) {
    if (c.arm < 0.55f || std::hypot(c.pos.x - player_.pos.x, c.pos.y - player_.pos.y) > 80.0) continue;
    for (const auto& st : c.sets) {
      const double th = st.arm_hd * DEG2RAD, len = st.arm_len * std::min(1.0f, c.arm);
      crossing_walls_.insert(crossing_walls_.end(), {static_cast<float>(st.pos.x), static_cast<float>(st.pos.y),
                                                     static_cast<float>(st.pos.x + std::sin(th) * len), static_cast<float>(st.pos.y + std::cos(th) * len),
                                                     static_cast<float>(st.pos.z + 0.15), static_cast<float>(st.pos.z + 1.15)});
    }
  }
  if (!tolls_.empty()) updateTollBars(std::min(GetFrameTime(), 0.1f), crossing_walls_);  // (the ETC bars too)
  addHomeDoorWall(crossing_walls_);  // (the flat's front door, locked until it is rented)
  driving_.setExtraWalls(&crossing_walls_);
  std::vector<V3> on_tracks;
  if (driving_.active() || driving_.hasCar()) {
    const Vehicle& v = driving_.car();
    const double half = Traffic::lengthOf(v.type) * 0.5;
    for (double t : {-1.0, 0.0, 1.0}) on_tracks.push_back({v.pos.x + std::sin(v.yaw) * half * t, v.pos.y + std::cos(v.yaw) * half * t, v.pos.z});
  }
  if (!driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0 && !flying_ && !player_.fly) on_tracks.push_back(player_.pos);
  trains_.markObstacles(on_tracks);
}

// IC ticket gates: the lane the player walks into, and from which side.
void App::updateStationGates(float dt) {
  gate_walls_ = crossing_walls_;
  if (gate_closed_t_ > 0.0f) gate_closed_t_ -= dt;
  if (gate_flash_t_ > 0.0f) gate_flash_t_ -= dt;
  const auto& gates = trains_.gates();
  if (gate_closed_t_ > 0.0f && gate_closed_gate_ >= 0 && gate_closed_gate_ < static_cast<int>(gates.size())) {
    // the shut flaps across the lane (collision)
    const StationGate& G = gates[static_cast<size_t>(gate_closed_gate_)];
    const double th = G.heading * DEG2RAD, rx = std::cos(th), ry = -std::sin(th);
    const double v = G.lanes[static_cast<size_t>(gate_closed_lane_)];
    const double h = StationGate::kLaneHalf + 0.1;
    gate_walls_.insert(gate_walls_.end(), {static_cast<float>(G.pos.x + rx * (v - h)), static_cast<float>(G.pos.y + ry * (v - h)),
                                           static_cast<float>(G.pos.x + rx * (v + h)), static_cast<float>(G.pos.y + ry * (v + h)),
                                           static_cast<float>(G.pos.z - 0.5), static_cast<float>(G.pos.z + 1.0)});
  }
  if (ride_train_ >= 0 || player_.fly || driving_.active()) return;
  int in_g = -1, in_l = -1;
  double in_u = 0.0;
  for (size_t gi = 0; gi < gates.size(); ++gi) {
    const StationGate& G = gates[gi];
    if (std::hypot(G.pos.x - player_.pos.x, G.pos.y - player_.pos.y) > 8.0 || std::fabs(player_.pos.z - G.pos.z) > 1.5) continue;
    const double th = G.heading * DEG2RAD;
    const double dx = player_.pos.x - G.pos.x, dy = player_.pos.y - G.pos.y;
    const double u = dx * std::sin(th) + dy * std::cos(th), v = dx * std::cos(th) - dy * std::sin(th);
    if (std::fabs(u) > StationGate::kHalfLen + 0.35) continue;
    for (size_t li = 0; li < G.lanes.size(); ++li)
      if (std::fabs(v - G.lanes[li]) < StationGate::kLaneHalf) {
        in_g = static_cast<int>(gi);
        in_l = static_cast<int>(li);
        in_u = u;
      }
  }
  if (in_g == gate_in_ && in_l == gate_lane_in_) return;
  gate_in_ = in_g;
  gate_lane_in_ = in_l;
  if (in_g < 0) return;
  const StationGate& G = gates[static_cast<size_t>(in_g)];
  const std::string st_name = G.station >= 0 && G.station < static_cast<int>(trains_.stations().size())
                                  ? trains_.stations()[static_cast<size_t>(G.station)].name : std::string();
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  auto shut = [&]() {
    gate_closed_gate_ = in_g;
    gate_closed_lane_ = in_l;
    gate_closed_t_ = 2.2f;
    gate_flash_ok_ = false;
    audio_.cue(Cue::Beep, 0.7f);
  };
  gate_flash_gate_ = in_g;
  gate_flash_lane_ = in_l;
  gate_flash_t_ = 0.9f;
  gate_flash_ok_ = true;
  if (in_u < 0.0) {  // from the free side: touch in
    const int64_t min_fare = railFare(false, 1.0);
    if (ledger_ && bal < min_fare) {
      shut();
      toast(tr("rail.no_money"));
      return;
    }
    in_paid_ = true;
    paid_km_ = 0.0;
    paid_shink_ = false;
    gate_owe_ = false;
    audio_.cue(Cue::GateBeep, 0.5f);
    toast(i18n_.f("rail.gate_in", {{"station", st_name}, {"bal", withCommas(bal)}}));
    TraceLog(LOG_INFO, "RJ: touched in at %s (balance %lld)", st_name.c_str(), static_cast<long long>(bal));
    return;
  }
  // from the paid side: touch out, the fare for the distance ridden
  if (paid_km_ < 0.05) {
    in_paid_ = false;
    audio_.cue(Cue::GateBeep, 0.5f);
    toast(i18n_.f("rail.gate_out_free", {{"station", st_name}, {"bal", withCommas(bal)}}));
    return;
  }
  const double km = std::max(1.0, paid_km_);
  const int64_t fare = railFare(paid_shink_, km);
  if (ledger_ && bal < fare) {
    if (!gate_owe_) {  // the flaps shut; the next touch lets the player through (no fare adjustment machine)
      gate_owe_ = true;
      shut();
      toast(i18n_.f("rail.gate_short", {{"fare", withCommas(fare)}, {"bal", withCommas(bal)}}));
      return;
    }
    if (bal > 0) ledger_->transfer(player_account_, ledger_->externalAccount(), bal, rj::econ::TxCategory::Fare, clock_.unixUtc(), st_name);
    toast(i18n_.f("rail.gate_waived", {{"paid", withCommas(bal)}, {"short", withCommas(fare - bal)}}));
  } else {
    if (ledger_)
      ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(), st_name);
    toast(i18n_.f("rail.gate_out", {{"station", st_name}, {"km", std::to_string(static_cast<int>(km + 0.5))}, {"fare", withCommas(fare)},
                                    {"bal", withCommas(ledger_ ? ledger_->balance(player_account_) : 0)}}));
  }
  TraceLog(LOG_INFO, "RJ: touched out at %s: %.1f km, fare %lld%s", st_name.c_str(), km, static_cast<long long>(fare), paid_shink_ ? " (Shinkansen)" : "");
  in_paid_ = false;
  paid_km_ = 0.0;
  paid_shink_ = false;
  gate_owe_ = false;
  audio_.cue(Cue::GateBeep, 0.5f);
}

void App::updateTransportActions() {
  if (!trains_.loaded() || !inside_id_.empty() || driving_.active() || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_) return;
  if (drive_train_ >= 0) return;
  if (ride_train_ >= 0) {
    const Train* t = trains_.train(ride_train_);
    if (!t) {
      ride_train_ = -1;
      return;
    }
    // what the car's displays say, in a quiet line at the top of the screen
    const auto& stations = trains_.stations();
    const std::string kmh = std::to_string(static_cast<int>(t->v * 3.6));
    if (t->at_station >= 0)
      ride_info_ = i18n_.f("rail.at", {{"line", lineName(t->line)}, {"station", stations[static_cast<size_t>(t->at_station)].name}});
    else if (t->next_stop >= 0)
      ride_info_ = i18n_.f("rail.next", {{"line", lineName(t->line)}, {"station", stations[static_cast<size_t>(t->next_stop)].name}, {"kmh", kmh}});
    else
      ride_info_ = i18n_.f("rail.running", {{"line", lineName(t->line)}, {"kmh", kmh}});
    updateSeatChoice();
    return;
  }
  updateBoarding();
  // left the station some other way (flying off, the map's travel): the card is no longer inside
  if (in_paid_ && trains_.stationNear(player_.pos, 400.0) < 0) in_paid_ = false, paid_km_ = 0.0;
}

// ------------------------------------------------------------------------------------------------
// Shared: what the crosshair is on

int App::aimSeat(const std::vector<SeatSlot>& seats, float px, float py, float look_yaw, float look_pitch, float eye_h, float reach) const {
  const float dx = std::sin(look_yaw) * std::cos(look_pitch), dy = std::cos(look_yaw) * std::cos(look_pitch), dz = std::sin(look_pitch);
  int best = -1;
  float best_c = std::cos(13.0f * DEG2RAD);
  for (size_t i = 0; i < seats.size(); ++i) {
    const float ox = seats[i].x - px, oy = seats[i].y - py, oz = 0.5f - eye_h;
    const float d = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (std::hypot(ox, oy) > reach || d < 0.05f) continue;
    const float c = (ox * dx + oy * dy + oz * dz) / d;
    if (c > best_c) best_c = c, best = static_cast<int>(i);
  }
  return best;
}

bool App::aimAt(const V3& target, double max_dist, double max_deg) const {
  const V3 e = player_.eyeEnu(), f = player_.forwardEnu();
  const V3 d{target.x - e.x, target.y - e.y, target.z - e.z};
  const double l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (l > max_dist || l < 1e-3) return l < 1e-3;
  return (d.x * f.x + d.y * f.y + d.z * f.z) / l > std::cos(max_deg * DEG2RAD);
}

// ------------------------------------------------------------------------------------------------
// Ferries: up the gangway from the pier (the fare is paid stepping aboard), about the open deck,
// on the benches, and down the gangway again at the other end.

bool App::ferryGangway(const Ferry& f, V3& deck_end, V3& pier_end, int& side, float& gy) const {
  if (f.phase != Ferry::Phase::Docked || f.timer <= 4.0) return false;
  const int pi = ferries_.currentPier(f);
  if (pi < 0) return false;
  const ShipClass& C = Ferries::shipClass(f.cls);
  const V3 gp = ferries_.gangwayPier(f);
  double gx, gyy;
  ferries_.toShip(f, gp, gx, gyy);
  side = gx >= 0 ? 1 : -1;
  gy = static_cast<float>(std::clamp(gyy, static_cast<double>(C.deck_y0) + 2.0, static_cast<double>(C.deck_y1) - 2.0));
  deck_end = ferries_.toWorld(f, side * (C.deck_x + 0.45), gy, C.deck_z);
  const double pz = ferries_.piers()[static_cast<size_t>(pi)].pos.z + 2.4;
  const double rise = deck_end.z - pz, run = std::max(3.0, rise * 1.35);
  const double ox = std::cos(f.yaw) * side, oy = -std::sin(f.yaw) * side;  // out of the ship's side
  pier_end = {deck_end.x + ox * run, deck_end.y + oy * run, pz};
  return true;
}

void App::updateFerryAboard(float dt) {
  dt = std::min(dt, 0.1f);
  const Ferry* f = ferries_.ship(ride_ferry_);
  if (!f) {
    ride_ferry_ = -1;
    return;
  }
  const ShipClass& C = Ferries::shipClass(f->cls);
  const bool input = screen_ == Screen::Game;
  const bool test = ride_test_t_ >= 0.0f && !ride_test_done_;
  if (input) {
    const Vector2 md = GetMouseDelta();
    const float sens = 0.0022f * settings_.mouse_sensitivity;
    ferry_look_yaw_ = wrapPi(ferry_look_yaw_ + md.x * sens);
    player_.pitch = std::clamp(player_.pitch + (settings_.invert_y ? md.y : -md.y) * sens, -1.4f, 1.4f);
  }
  V3 D, G;
  int side = 1;
  float gy = 0.0f;
  const bool gang = ferryGangway(*f, D, G, side, gy);
  // where the player wants to go (ship frame)
  float wx = 0.0f, wy = 0.0f;
  bool run = false;
  if (input && !test) {
    const float a = ferry_look_yaw_;
    const float fwd = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.0f : 0.0f) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.0f : 0.0f);
    const float sd = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
    wx = std::sin(a) * fwd + std::cos(a) * sd;
    wy = std::cos(a) * fwd - std::sin(a) * sd;
    run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  }
  if (test && !ferry_test_walked_) {  // test aid: up the gangway, a few steps forward along the side deck, look out
    if (ferry_gang_ >= 0.0) {
      wx = static_cast<float>(-side);
    } else {
      const float tx = static_cast<float>(side) * (C.deck_x - 0.5f), ty = gy + 3.0f;
      const float dx = tx - static_cast<float>(ferry_x_), dy = ty - static_cast<float>(ferry_y_), d = std::hypot(dx, dy);
      if (d < 0.1f) {
        ferry_test_walked_ = true;
        ferry_look_yaw_ = side > 0 ? 0.35f : -0.35f;  // forward along the side deck, a little out to sea
      } else {
        wx = dx / d, wy = dy / d;
      }
    }
  }
  const float wl = std::hypot(wx, wy);
  if (wl > 1e-4f) wx /= wl, wy /= wl;
  const bool moving = wl > 1e-4f;
  const float spd = (run ? 2.8f : 1.4f) * dt;
  const double sfx = std::sin(f->yaw), sfy = std::cos(f->yaw), srx = sfy, sry = -sfx;
  V3 feet;
  float sit_drop = 0.0f;
  if (ferry_gang_ >= 0.0) {
    if (!gang) {  // the gangway is being taken in: off it at the nearer end
      if (ferry_gang_ > 0.5) {  // (onto the pier where the player stands)
        ride_ferry_ = -1;
        ferry_gang_ = -1.0;
        player_.vel_z = 0;
        player_.snapToGround(world_);
        return;
      }
      ferry_gang_ = -1.0;
      ferry_x_ = side * (C.deck_x - 0.4);
      ferry_y_ = gy;
    } else {
      const double wX = srx * wx + sfx * wy, wY = sry * wx + sfy * wy;  // wish in the world
      const double ux = G.x - D.x, uy = G.y - D.y, ul = std::max(1e-3, std::hypot(ux, uy));
      const double slope_len = std::hypot(ul, D.z - G.z);
      ferry_gang_ += (wX * ux + wY * uy) / ul * spd / slope_len;
      if (ferry_gang_ >= 1.0) {  // stepped off onto the pier
        const int at = ferries_.currentPier(*f);
        ride_ferry_ = -1;
        ferry_gang_ = -1.0;
        player_.pos = G;
        player_.vel_z = 0;
        player_.fly = false;
        player_.yaw = f->yaw + ferry_look_yaw_;
        player_.snapToGround(world_);
        if (ferry_paid_ && at >= 0) toast(i18n_.f("ferry.alighted", {{"pier", pierName(at)}}));
        ferry_paid_ = false;
        return;
      }
      if (ferry_gang_ <= 0.0) {  // stepping aboard: the fare
        if (!ferry_paid_) {
          const int64_t fare = f->cruise > 15.0 ? 4200 : f->cls == 0 ? 480 : 2600;  // game fares by service
          if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(),
                                            tr("ferry.name")) != rj::econ::TxResult::Ok) {
            toast(tr("rail.no_money"));
            ferry_gang_ = 0.08;
          } else {
            ferry_paid_ = true;
            toast(i18n_.f("ferry.boarded", {{"fare", withCommas(fare)}}));
          }
        }
        if (ferry_paid_) {
          ferry_gang_ = -1.0;
          ferry_x_ = side * (C.deck_x - 0.4);
          ferry_y_ = gy;
        }
      }
    }
    if (ferry_gang_ >= 0.0) {
      const double g = ferry_gang_;
      feet = {D.x + (G.x - D.x) * g, D.y + (G.y - D.y) * g, D.z + (G.z - D.z) * g};
    }
  }
  if (ferry_gang_ < 0.0) {
    const auto seats = ferrySeats(C);
    if (ferry_sitting_) {
      ferry_sit_ = std::min(1.0f, ferry_sit_ + dt * 2.2f);
      if (ferry_sit_ < 1.0f && ferry_seat_ >= 0) ferry_look_yaw_ += wrapPi(seats[static_cast<size_t>(ferry_seat_)].facing - ferry_look_yaw_) * std::min(1.0f, dt * 5.0f);
      if (ferry_sit_ >= 1.0f && moving) ferry_sitting_ = false;
    } else {
      ferry_sit_ = std::max(0.0f, ferry_sit_ - dt * 2.6f);
      if (ferry_sit_ <= 0.0f) {
        ferry_seat_ = -1;
        if (moving) {
          const double nx = ferry_x_ + wx * spd, ny = ferry_y_ + wy * spd;
          if (ferryDeckWalkable(C, nx, ny)) ferry_x_ = nx, ferry_y_ = ny;
          else if (ferryDeckWalkable(C, nx, ferry_y_)) ferry_x_ = nx;
          else if (ferryDeckWalkable(C, ferry_x_, ny)) ferry_y_ = ny;
        }
        // onto the gangway (docked, at its head, stepping out)
        if (gang && ferry_x_ * side > C.deck_x - 0.45 && std::fabs(ferry_y_ - gy) < 0.8 && wx * side > 0.3f) ferry_gang_ = 0.01;
      }
    }
    double px = ferry_x_, py = ferry_y_;
    if (ferry_seat_ >= 0 && ferry_seat_ < static_cast<int>(seats.size())) {
      const float k = ferry_sit_ * ferry_sit_ * (3.0f - 2.0f * ferry_sit_);
      px += (seats[static_cast<size_t>(ferry_seat_)].x - px) * k;
      py += (seats[static_cast<size_t>(ferry_seat_)].y - py) * k;
      sit_drop = 0.46f * k;
    }
    feet = ferries_.toWorld(*f, px, py, C.deck_z);
    // benches: look at a free seat and sit down (E / click), E or a step to stand up
    if (input && ferry_gang_ < 0.0) {
      if (ferry_sitting_) {
        if (ferry_sit_ >= 1.0f) {
          aim_icon_ = AimIcon::Stand;
          aim_label_ = tr("aim.stand");
          if (usePressed()) ferry_sitting_ = false;
        }
      } else if (ferry_sit_ <= 0.0f) {
        const int si = aimSeat(seats, static_cast<float>(ferry_x_), static_cast<float>(ferry_y_), ferry_look_yaw_, player_.pitch, kStandEye, 1.6f);
        if (si >= 0) {
          const bool taken = Crowd::ferrySeatTaken(*f, si);
          aim_icon_ = taken ? AimIcon::Blocked : AimIcon::Seat;
          aim_label_ = tr(taken ? "aim.seat_taken" : "aim.sit");
          if (!taken && usePressed()) {
            ferry_seat_ = si;
            ferry_sitting_ = true;
          }
        }
      }
    }
  }
  player_.pos = {feet.x, feet.y, feet.z - sit_drop};
  player_.yaw = f->yaw + ferry_look_yaw_;
  player_.cam_z = player_.pos.z;
  player_.cam_z_init = true;
  player_.bob_amount = 0.0f;
  if (f->phase == Ferry::Phase::Offmap) ferries_.fastForwardOffmap(f->id);
  // the voyage in a quiet line at the top
  const std::string kn = std::to_string(static_cast<int>(std::lround(std::fabs(f->v) * 1.944)));
  const int dest = ferries_.destinationPier(*f);
  if (f->phase == Ferry::Phase::Offmap) ride_info_ = tr("ferry.mainland");
  else if (f->phase == Ferry::Phase::Docked && ferries_.currentPier(*f) >= 0) ride_info_ = i18n_.f("ferry.docked", {{"pier", pierName(ferries_.currentPier(*f))}});
  else ride_info_ = dest >= 0 ? i18n_.f("ferry.under_way", {{"pier", pierName(dest)}, {"kn", kn}}) : i18n_.f("ferry.to_mainland", {{"kn", kn}});
}

void App::updateFerryActions() {
  if (!ferries_.loaded() || !inside_id_.empty() || driving_.active() || ride_train_ >= 0 || ride_jet_ >= 0 || flying_ || ride_ferry_ >= 0) return;
  if (player_.fly) return;
  // on foot at the foot of a gangway, stepping towards the ship: onto it
  for (const auto& f : ferries_.ships()) {
    V3 D, G;
    int side;
    float gy;
    if (!ferryGangway(f, D, G, side, gy)) continue;
    if (std::hypot(G.x - player_.pos.x, G.y - player_.pos.y) > 1.3 || std::fabs(G.z - player_.pos.z) > 1.5) continue;
    const double ux = D.x - G.x, uy = D.y - G.y, ul = std::max(1e-3, std::hypot(ux, uy));
    if ((player_.wish.x * ux + player_.wish.y * uy) / ul < 0.3) continue;
    ride_ferry_ = f.id;
    ferry_gang_ = 0.97;
    ferry_paid_ = false;
    ferry_seat_ = -1;
    ferry_sitting_ = false;
    ferry_sit_ = 0.0f;
    ferry_look_yaw_ = wrapPi(player_.yaw - f.yaw);
    TraceLog(LOG_INFO, "RJ: stepped onto the gangway of ferry %d", f.id);
    return;
  }
  // test aid (--state ferry): walk from the pier to the gangway
  if (ride_test_t_ >= 0.0f && !ride_test_done_ && ride_test_t_ <= 0.0f && opt_.state == "ferry") {
    for (const auto& f : ferries_.ships()) {
      V3 D, G;
      int side;
      float gy;
      if (!ferryGangway(f, D, G, side, gy) || std::hypot(G.x - player_.pos.x, G.y - player_.pos.y) > 60.0) continue;
      const V3 aim{G.x + (D.x - G.x) * 0.3, G.y + (D.y - G.y) * 0.3, G.z};
      player_.yaw = static_cast<float>(std::atan2(aim.x - player_.pos.x, aim.y - player_.pos.y));
      player_.auto_forward_s = 0.05f;
      break;
    }
  }
}

// ------------------------------------------------------------------------------------------------
// The airliner: up the passenger stairs at the stand (the fare is paid at the door), along the
// aisle to a free seat; the flight leaves once the player has sat down. Down the stairs at the end.

V3 App::jetToWorld(const Airliner& a, float x, float y, float z) const {
  const Vector3 p = enuToRl(a.pos);
  const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a.roll), MatrixRotateX(a.pitch)), MatrixRotateY(-a.yaw)),
                                  MatrixTranslate(p.x, p.y, p.z));
  return rlToEnu(Vector3Transform({x, z, -y}, M));  // model (x, y, z) -> raylib (x, z, -y)
}

namespace {
constexpr float kJetDoorMid = (kJetDoorY0 + kJetDoorY1) * 0.5f;
constexpr float kJetLanding = 0.8f;  // the stairs' top landing (from the skin outwards)
// a point on the stairs (model frame) at s: 0 in the doorway .. 1 on the apron at their foot
void jetStairPoint(double s, float& x, float& z) {
  const float xs = -kJetSkinX - 0.05f, rise = kJetFloorZ + 2.4f;
  const float slope = std::hypot(kJetStairRun, rise);
  const float total = kJetLanding + slope + 0.3f;
  float d = static_cast<float>(std::clamp(s, 0.0, 1.0)) * total;
  if (d <= kJetLanding) {
    x = xs - d;
    z = kJetFloorZ;
  } else if (d <= kJetLanding + slope) {
    const float t = (d - kJetLanding) / slope;
    x = xs - kJetLanding - kJetStairRun * t;
    z = kJetFloorZ - rise * t;
  } else {
    x = xs - kJetLanding - kJetStairRun - (d - kJetLanding - slope);
    z = -2.4f;
  }
}
float jetStairLength() { return kJetLanding + std::hypot(kJetStairRun, kJetFloorZ + 2.4f) + 0.3f; }
}  // namespace

void App::updateJetAboard(float dt) {
  dt = std::min(dt, 0.1f);
  const Airliner* a = aviation_.airliner(ride_jet_);
  if (!a) {
    ride_jet_ = -1;
    return;
  }
  if (a->phase != Airliner::Phase::AtStand) jet_flown_ = true;
  if (a->phase == Airliner::Phase::Offmap) aviation_.fastForwardOffmap(a->id);
  const bool input = screen_ == Screen::Game;
  const bool test = ride_test_t_ >= 0.0f && !ride_test_done_;
  if (input) {
    const Vector2 md = GetMouseDelta();
    const float sens = 0.0022f * settings_.mouse_sensitivity;
    jet_look_yaw_ = wrapPi(jet_look_yaw_ + md.x * sens);
    jet_look_pitch_ = std::clamp(jet_look_pitch_ + (settings_.invert_y ? md.y : -md.y) * sens, -1.3f, 1.3f);
  }
  const bool door_open = a->phase == Airliner::Phase::AtStand;
  float wx = 0.0f, wy = 0.0f, reach = 1e9f;
  bool run = false;
  if (input && !test) {
    const float yw = jet_look_yaw_;
    const float fwd = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.0f : 0.0f) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.0f : 0.0f);
    const float sd = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
    wx = std::sin(yw) * fwd + std::cos(yw) * sd;
    wy = std::cos(yw) * fwd - std::sin(yw) * sd;
    run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  }
  if (!jet_path_.empty() && !jet_sitting_) {  // test aid
    const auto [tx, ty] = jet_path_.front();
    const float dx = tx - jet_x_, dy = ty - jet_y_, d = std::hypot(dx, dy);
    if (d < 0.06f) jet_path_.erase(jet_path_.begin());
    else wx = dx / d, wy = dy / d, reach = d, jet_look_yaw_ = std::atan2(dx, dy);
  }
  if (test && jet_stair_ >= 0.0 && !jet_flown_) wx = 1.0f, wy = 0.0f;  // test aid: up the stairs
  const float wl = std::hypot(wx, wy);
  if (wl > 1e-4f) wx /= wl, wy /= wl;
  const bool moving = wl > 1e-4f;
  const float spd = std::min((run ? 2.4f : 1.3f) * dt, reach);
  V3 feet;
  float eye_drop = 0.0f;
  if (jet_stair_ >= 0.0) {
    if (!door_open) {  // (the door shuts: inside)
      jet_stair_ = -1.0;
      jet_x_ = -1.1f;
      jet_y_ = kJetDoorMid;
    } else {
      jet_stair_ += -wx * spd / jetStairLength();  // outwards (model -x) is down the stairs
      if (jet_stair_ >= 1.0) {                    // on the apron
        float x, z;
        jetStairPoint(1.0, x, z);
        const V3 w = jetToWorld(*a, x, kJetDoorMid, z);
        aviation_.setAboard(a->id, false);
        aviation_.setSeated(a->id, false);
        ride_jet_ = -1;
        jet_stair_ = -1.0;
        player_.pos = w;
        player_.vel_z = 0;
        player_.fly = false;
        player_.yaw = a->yaw + jet_look_yaw_;
        player_.snapToGround(world_);
        if (jet_paid_) toast(jet_flown_ ? i18n_.f("jet.alighted", {{"airport", airportName(a->from)}}) : tr("jet.left_before"));
        jet_paid_ = false;
        return;
      }
      if (jet_stair_ <= 0.0) {  // through the door: the fare
        if (!jet_paid_) {
          const int64_t fare = 12800;  // game value
          if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(),
                                            tr("jet.airline")) != rj::econ::TxResult::Ok) {
            toast(tr("rail.no_money"));
            jet_stair_ = 0.05;
          } else {
            jet_paid_ = true;
            toast(i18n_.f("jet.boarded", {{"fare", withCommas(fare)}, {"dest", airportName(a->to)}}));
          }
        }
        if (jet_paid_) {
          jet_stair_ = -1.0;
          jet_x_ = -1.3f;
          jet_y_ = kJetDoorMid;
        }
      }
    }
    if (jet_stair_ >= 0.0) {
      float x, z;
      jetStairPoint(jet_stair_, x, z);
      feet = jetToWorld(*a, x, kJetDoorMid, z);
    }
  }
  if (jet_stair_ < 0.0) {
    const auto seats = jetSeats();
    if (jet_sitting_) {
      jet_sit_ = std::min(1.0f, jet_sit_ + dt * 2.2f);
      if (jet_sit_ < 1.0f) jet_look_yaw_ += wrapPi(0.0f - jet_look_yaw_) * std::min(1.0f, dt * 5.0f);
      if (jet_sit_ >= 1.0f && moving && input) jet_sitting_ = false;
    } else {
      jet_sit_ = std::max(0.0f, jet_sit_ - dt * 2.6f);
      if (jet_sit_ <= 0.0f) {
        jet_seat_ = -1;
        if (moving) {
          const float nx = jet_x_ + wx * spd, ny = jet_y_ + wy * spd;
          if (jetCabinWalkable(nx, ny, door_open)) jet_x_ = nx, jet_y_ = ny;
          else if (jetCabinWalkable(nx, jet_y_, door_open)) jet_x_ = nx;
          else if (jetCabinWalkable(jet_x_, ny, door_open)) jet_y_ = ny;
        }
        if (door_open && jet_x_ < -kJetSkinX + 0.05f) jet_stair_ = 0.01;  // out of the door onto the landing
      }
    }
    float px = jet_x_, py = jet_y_;
    if (jet_seat_ >= 0 && jet_seat_ < static_cast<int>(seats.size())) {
      const float k = jet_sit_ * jet_sit_ * (3.0f - 2.0f * jet_sit_);
      px += (seats[static_cast<size_t>(jet_seat_)].x - px) * k;
      py += (seats[static_cast<size_t>(jet_seat_)].y - py) * k;
      eye_drop = (kStandEye - kSeatEye) * k;
    }
    feet = jetToWorld(*a, px, py, kJetFloorZ);
    if (input && jet_stair_ < 0.0) {
      if (jet_sitting_) {
        if (jet_sit_ >= 1.0f) {
          aim_icon_ = AimIcon::Stand;
          aim_label_ = tr("aim.stand");
          if (usePressed()) jet_sitting_ = false;
        }
      } else if (jet_sit_ <= 0.0f) {
        const int si = aimSeat(seats, jet_x_, jet_y_, jet_look_yaw_, jet_look_pitch_, kStandEye, 1.4f);
        if (si >= 0) {
          const bool taken = Crowd::jetSeatTaken(*a, si);
          aim_icon_ = taken ? AimIcon::Blocked : AimIcon::Seat;
          aim_label_ = tr(taken ? "aim.seat_taken" : "aim.sit");
          if (!taken && usePressed()) {
            jet_seat_ = si;
            jet_sitting_ = true;
          }
        }
      }
    }
  }
  aviation_.setSeated(a->id, jet_sitting_ && jet_sit_ >= 1.0f);
  player_.pos = feet;
  player_.yaw = a->yaw + jet_look_yaw_;
  player_.pitch = jet_look_pitch_;
  player_.cam_z = feet.z;
  player_.cam_z_init = true;
  player_.bob_amount = 0.0f;
  (void)eye_drop;
  // the flight in a quiet line at the top
  const Airport& ap = aviation_.airports()[static_cast<size_t>(a->from)];
  const std::string kt = std::to_string(static_cast<int>(a->v * 1.944));
  const std::string ft = std::to_string(static_cast<int>(std::max(0.0, (a->pos.z - 2.4 - ap.rwy_a.z) * 3.281) / 100.0) * 100);
  using P = Airliner::Phase;
  switch (a->phase) {
    case P::AtStand:
      ride_info_ = jet_flown_ ? i18n_.f("jet.arrived", {{"airport", airportName(a->from)}})
                   : (a->timer <= 0.0 && !(jet_sitting_ && jet_sit_ >= 1.0f)) ? tr("jet.sit_please") : i18n_.f("jet.at_stand", {{"dest", airportName(a->to)}});
      break;
    case P::Pushback:
    case P::TaxiOut: ride_info_ = tr("jet.taxi_out"); break;
    case P::Takeoff: ride_info_ = i18n_.f("jet.takeoff", {{"kt", kt}}); break;
    case P::Climb: ride_info_ = i18n_.f("jet.climb", {{"ft", ft}, {"kt", kt}}); break;
    case P::Offmap: ride_info_ = tr("jet.mainland"); break;
    case P::Approach: ride_info_ = i18n_.f("jet.approach", {{"ft", ft}, {"kt", kt}, {"airport", airportName(a->to)}}); break;
    case P::Landing: ride_info_ = i18n_.f("jet.landing", {{"kt", kt}}); break;
    case P::TaxiIn: ride_info_ = tr("jet.taxi_in"); break;
  }
}

Camera3D App::jetCamera() const {
  const Airliner* a = aviation_.airliner(ride_jet_);
  if (!a) return player_.camera(settings_.fov);
  float px = jet_x_, py = jet_y_, pz = kJetFloorZ, eh = kStandEye;
  if (jet_stair_ >= 0.0) {
    jetStairPoint(jet_stair_, px, pz);
    py = kJetDoorMid;
  } else if (jet_seat_ >= 0) {
    const auto seats = jetSeats();
    const float k = jet_sit_ * jet_sit_ * (3.0f - 2.0f * jet_sit_);
    const SeatSlot& s = seats[static_cast<size_t>(jet_seat_)];
    px += (s.x - px) * k;
    py += (s.y + 0.12f - py) * k;  // (the head a little ahead of the seat back)
    eh += (kSeatEye - kStandEye) * k;
  }
  const V3 eye = jetToWorld(*a, px, py, pz + eh);
  const float ly = jet_look_yaw_, lp = jet_look_pitch_;
  const V3 tgt = jetToWorld(*a, px + std::sin(ly) * std::cos(lp), py + std::cos(ly) * std::cos(lp), pz + eh + std::sin(lp));
  const V3 up = jetToWorld(*a, px, py, pz + eh + 1.0f);
  Camera3D c{};
  c.position = enuToRl(eye);
  c.target = enuToRl(tgt);
  c.up = Vector3Normalize(Vector3Subtract(enuToRl(up), c.position));
  c.fovy = settings_.fov;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

void App::updateAviationActions() {
  if (!aviation_.loaded() || !inside_id_.empty() || driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_) return;
  if (player_.fly) return;
  // on foot at the foot of the passenger stairs, stepping towards the aircraft: up them
  for (const auto& a : aviation_.airliners()) {
    if (a.phase != Airliner::Phase::AtStand || a.timer < 5.0) continue;
    float fx, fz;
    jetStairPoint(1.0, fx, fz);
    const V3 foot = jetToWorld(a, fx, kJetDoorMid, fz);
    if (std::hypot(foot.x - player_.pos.x, foot.y - player_.pos.y) > 1.3 || std::fabs(foot.z - player_.pos.z) > 1.5) continue;
    const V3 in = jetToWorld(a, fx + 1.0f, kJetDoorMid, fz);
    const double ux = in.x - foot.x, uy = in.y - foot.y, ul = std::max(1e-3, std::hypot(ux, uy));
    if ((player_.wish.x * ux + player_.wish.y * uy) / ul < 0.3) continue;
    ride_jet_ = a.id;
    jet_stair_ = 0.97;
    jet_paid_ = false;
    jet_flown_ = false;
    jet_seat_ = -1;
    jet_sitting_ = false;
    jet_sit_ = 0.0f;
    jet_look_yaw_ = wrapPi(player_.yaw - a.yaw);
    jet_look_pitch_ = player_.pitch;
    aviation_.setAboard(a.id, true);
    aviation_.setSeated(a.id, false);
    TraceLog(LOG_INFO, "RJ: stepped onto the stairs of flight %d", a.id);
    return;
  }
  // the light aircraft: look at it and get in (E / click)
  const LightPlane& pl = aviation_.plane();
  if (!pl.crashed() && pl.onGround() && std::hypot(player_.pos.x - pl.pos().x, player_.pos.y - pl.pos().y) < 6.5 &&
      std::fabs(player_.pos.z - pl.pos().z) < 3.0 && aimAt({pl.pos().x, pl.pos().y, pl.pos().z}, 8.0, 28.0)) {
    aim_icon_ = AimIcon::Enter;
    aim_label_ = tr("aim.fly");
    if (usePressed()) {
      flying_ = true;
      fly_cockpit_ = true;
      fly_look_yaw_ = fly_look_pitch_ = 0.0f;
      plane_in_ = PlaneControls{};
      plane_in_.brake = true;
      toast(tr("fly.entered"));
    }
  }
}

void App::jetTestPilot() {
  if (opt_.state != "jet" || ride_test_t_ < 0.0f || ride_test_done_) return;
  if (ride_jet_ < 0) {
    if (ride_test_t_ > 0.0f) return;
    for (const auto& a : aviation_.airliners()) {
      if (a.phase != Airliner::Phase::AtStand || a.from != 0 || a.timer < 5.0) continue;
      float fx, fz;
      jetStairPoint(1.0, fx, fz);
      const V3 foot = jetToWorld(a, fx, kJetDoorMid, fz);
      const V3 out = jetToWorld(a, fx - 5.0f, kJetDoorMid, fz);
      if (std::hypot(foot.x - player_.pos.x, foot.y - player_.pos.y) > 30.0) {  // (test aid: stand a few steps from the stairs)
        player_.pos = out;
        player_.snapToGround(world_);
      }
      player_.yaw = static_cast<float>(std::atan2(foot.x - player_.pos.x, foot.y - player_.pos.y));
      player_.auto_forward_s = 0.05f;
      if (frame_ % 30 == 0)
        TraceLog(LOG_INFO, "RJ: jet test to the stairs of flight %d: %.1f m, dz %.2f (floor %.2f terrain %.2f grounded %d) at %.6f %.6f, cells %d/%d pending %d", a.id,
                 std::hypot(foot.x - player_.pos.x, foot.y - player_.pos.y), foot.z - player_.pos.z,
                 world_.floorBelow(player_.pos.x, player_.pos.y, player_.pos.z + 0.55).value_or(-999.0),
                 world_.terrainHeight(player_.pos.x, player_.pos.y).value_or(-999.0), player_.grounded ? 1 : 0,
                 world_.toGeodetic(player_.pos).lat_deg, world_.toGeodetic(player_.pos).lon_deg, static_cast<int>(world_.residentCount()),
                 static_cast<int>(world_.knownCount()), static_cast<int>(world_.pendingJobs()));
      break;
    }
    return;
  }
  if (jet_test_seated_ && jet_sitting_ && jet_sit_ >= 1.0f && !jet_test_looked_) {  // out of the window (RJ_JET_LOOK: yaw, pitch)
    jet_test_looked_ = true;
    jet_look_yaw_ = -1.25f;
    jet_look_pitch_ = -0.12f;
    if (const char* e = std::getenv("RJ_JET_LOOK")) std::sscanf(e, "%f,%f", &jet_look_yaw_, &jet_look_pitch_);
  }
  if (jet_stair_ >= 0.0 || jet_sitting_ || jet_sit_ > 0.0f || !jet_path_.empty() || jet_test_seated_) return;
  const Airliner* a = aviation_.airliner(ride_jet_);
  if (!a) return;
  // the free left window seat nearest the wing (about y = -2)
  const auto seats = jetSeats();
  int best = -1;
  float bd = 1e9f;
  for (size_t i = 0; i < seats.size(); ++i) {
    if (seats[i].x > -0.9f || Crowd::jetSeatTaken(*a, static_cast<int>(i))) continue;
    const float d = std::fabs(seats[i].y + 2.1f);
    if (d < bd) bd = d, best = static_cast<int>(i);
  }
  if (best < 0) return;
  const SeatSlot& s = seats[static_cast<size_t>(best)];
  if (std::hypot(s.fx - jet_x_, s.fy - jet_y_) < 0.08f) {
    jet_seat_ = best;
    jet_sitting_ = true;
    jet_test_seated_ = true;
    TraceLog(LOG_INFO, "RJ: jet test sits on seat %d", best);
    return;
  }
  jet_path_ = {{0.0f, jet_y_}, {0.0f, s.fy}, {s.fx, s.fy}};
}

// test aid (--state ride): walk to the nearest open door, board, take the nearest free seat; with
// --alight, get up at the next stop and walk off through a door
void App::rideTestPilot() {
  if (ride_test_t_ < 0.0f || ride_test_done_ || ride_place_pending_ || drive_train_ >= 0) return;
  if (ride_train_ < 0) {
    if (ride_test_t_ > 0.0f) return;  // (alighted: done walking)
    double best = 1e30;
    V3 target{};
    for (const auto& t : trains_.trains()) {
      if (t.at_station != opt_.station || Trains::doorOpen(t) < 0.6f) continue;
      for (int k = 0; k < t.cars; ++k) {
        const CarFrame F = carFrame(trains_, t, k);
        const CarLayout L = carLayoutOf(trains_, t, k);
        const double side = doorSideLeft(t, k) ? -1.0 : 1.0;
        for (const auto& d : L.doors) {
          const V3 out = F.at(side * (L.half_w + 0.9), (d.first + d.second) * 0.5, 0.0);
          const double dist = std::hypot(out.x - player_.pos.x, out.y - player_.pos.y);
          if (dist < best) {
            best = dist;
            // from out on the platform, straight in through the door
            target = dist > 0.5 ? out : F.at(0.0, (d.first + d.second) * 0.5, 0.0);
          }
        }
      }
    }
    if (best < 60.0) {
      player_.yaw = static_cast<float>(std::atan2(target.x - player_.pos.x, target.y - player_.pos.y));
      player_.auto_forward_s = 0.05f;
    }
    return;
  }
  const Train* t = trains_.train(ride_train_);
  if (!t) return;
  const CarLayout L = carLayoutOf(trains_, *t, ride_car_);
  const float aisle = L.shink ? -0.28f : 0.0f;
  if (opt_.alight && ride_test_t_ >= opt_.ride) {
    if (t->at_station < 0 || Trains::doorOpen(*t) < 0.6f) return;
    if (ob_sitting_) {
      if (ob_sit_ >= 1.0f) ob_sitting_ = false;
      return;
    }
    if (ob_sit_ > 0.0f || !ob_path_.empty()) return;
    // the nearest door, and out
    float by = 0.0f, bd = 1e9f;
    for (const auto& d : L.doors) {
      const float yc = (d.first + d.second) * 0.5f;
      if (std::fabs(yc - ob_y_) < bd) bd = std::fabs(yc - ob_y_), by = yc;
    }
    const float side = doorSideLeft(*t, ride_car_) ? -1.0f : 1.0f;
    ob_path_ = {{aisle, ob_y_}, {aisle, by}, {side * (L.half_w + 0.6f), by}};
    return;
  }
  if (ob_test_seated_ && ob_sitting_ && ob_sit_ >= 1.0f && !ob_test_looked_) {  // the view the test asked for
    ob_test_looked_ = true;
    if (ob_seat_ >= 0 && ob_seat_ < static_cast<int>(L.seats.size())) {
      ride_look_yaw_ = L.seats[static_cast<size_t>(ob_seat_)].facing + ob_test_look_[0];
      ride_look_pitch_ = ob_test_look_[1];
    }
  }
  if (ob_sitting_ || ob_sit_ > 0.0f || !ob_path_.empty() || ob_test_seated_) return;
  int best = -1;
  float bd = 1e9f;
  for (size_t i = 0; i < L.seats.size(); ++i) {
    if (crowd_.seatTaken(trains_, t->id, ride_car_, static_cast<int>(i))) continue;
    const float d = std::hypot(L.seats[i].fx - ob_x_, L.seats[i].fy - ob_y_);
    if (d < bd) bd = d, best = static_cast<int>(i);
  }
  if (best < 0) {  // a full car: stand in the aisle
    ob_test_seated_ = true;
    ob_path_ = {{aisle, ob_y_}};
    return;
  }
  const SeatSlot& s = L.seats[static_cast<size_t>(best)];
  if (std::hypot(s.fx - ob_x_, s.fy - ob_y_) < 0.1f) {
    ob_seat_ = best;
    ob_sitting_ = true;
    ob_test_seated_ = true;
    // look out of the window (Shinkansen: to the side of a forward-facing seat; commuter: across the car)
    ob_test_look_[0] = L.shink ? (s.x < 0 ? -1.1f : 1.1f) : 0.0f;
    ob_test_look_[1] = -0.05f;
    if (const char* e = std::getenv("RJ_RIDE_LOOK")) std::sscanf(e, "%f,%f", &ob_test_look_[0], &ob_test_look_[1]);  // test aid (radians, relative to the seat)
    TraceLog(LOG_INFO, "RJ: ride test sits on seat %d of car %d", best, ride_car_);
    return;
  }
  ob_path_ = {{aisle, ob_y_}, {aisle, s.fy}, {s.fx, s.fy}};
}

}  // namespace rjc
