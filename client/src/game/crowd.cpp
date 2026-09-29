#include "game/crowd.hpp"

#include <algorithm>
#include <cmath>

#include "game/aircraft.hpp"
#include "game/car_layout.hpp"
#include "game/deck_layout.hpp"
#include "game/ferries.hpp"
#include "game/trains.hpp"
#include "raymath.h"
#include "render/aircraft.hpp"
#include "render/trains.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

using V3 = rj::geo::Vec3d;
constexpr double kPi = 3.14159265358979323846;

uint64_t mix(uint64_t x) {  // splitmix64
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
double u01(uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

void style(CrowdPerson& p, uint64_t seed, bool commute) {
  static const Color tops[] = {{235, 235, 232, 255}, {40, 52, 84, 255},  {30, 30, 32, 255},   {128, 130, 134, 255}, {196, 180, 150, 255},
                               {150, 180, 210, 255}, {96, 104, 72, 255}, {118, 40, 44, 255},  {60, 60, 64, 255},    {210, 206, 196, 255},
                               {70, 80, 60, 255},    {176, 150, 128, 255}};
  static const Color suits[] = {{32, 36, 48, 255}, {44, 46, 52, 255}, {26, 26, 28, 255}, {70, 72, 78, 255}};
  static const Color bottoms[] = {{28, 28, 30, 255}, {38, 46, 72, 255}, {96, 96, 100, 255}, {170, 150, 120, 255}, {60, 80, 120, 255}, {44, 40, 38, 255}};
  static const Color skins[] = {{236, 204, 176, 255}, {222, 186, 150, 255}, {204, 166, 132, 255}};
  static const Color hairs[] = {{22, 18, 16, 255}, {30, 24, 20, 255}, {58, 40, 28, 255}, {92, 66, 44, 255}, {150, 150, 150, 255}};
  const uint64_t h = mix(seed);
  const bool suit = commute && (h & 3) != 0;  // at rush hours most commuters wear dark suits
  p.top = suit ? suits[(h >> 4) % 4] : tops[(h >> 4) % 12];
  p.bottom = suit ? p.top : bottoms[(h >> 9) % 6];
  p.skin = skins[(h >> 14) % 3];
  p.hair = ((h >> 17) % 9 == 0) ? hairs[4] : hairs[(h >> 20) % 4];
  p.variant = static_cast<int>((h >> 24) % 3);
  p.scale = static_cast<float>(0.94 + 0.12 * u01(mix(h)));
}

// How busy the trains are (0 empty .. 1 packed): a game assumption shaped like Tokyo's day.
float busyAt(int hour, bool weekend) {
  if (weekend) return hour >= 10 && hour < 19 ? 0.45f : (hour >= 7 && hour < 23 ? 0.28f : 0.08f);
  if (hour >= 7 && hour < 9) return 1.0f;
  if (hour >= 17 && hour < 20) return 0.85f;
  if (hour >= 10 && hour < 17) return 0.42f;
  if (hour == 6 || hour == 9) return 0.6f;
  if (hour >= 20 && hour < 23) return 0.5f;
  return 0.1f;
}

using Frame = CarFrame;  // a car's pose as drawn (car_layout.hpp)

}  // namespace

namespace {
uint64_t carBase(const Train& t, int k, int trip) {
  return mix(static_cast<uint64_t>(t.id) * 131 + static_cast<uint64_t>(k) * 7919 + static_cast<uint64_t>(trip) * 104729);
}
double seatOccupancy(bool shink, float busy) { return shink ? 0.25 + 0.45 * busy : std::min(1.0, 0.08 + 1.05 * busy); }
}  // namespace

bool Crowd::seatTaken(const Trains& trains, int train_id, int k, int seat) const {
  const Train* t = trains.train(train_id);
  if (!t) return false;
  const bool shink = trains.lines()[static_cast<size_t>(t->line)].kind == LineKind::Shinkansen;
  auto it = trip_.find(t->id);
  const uint64_t base = carBase(*t, k, it == trip_.end() ? 0 : it->second);
  return u01(mix(base + static_cast<uint64_t>(seat) + 1)) < seatOccupancy(shink, busy_);
}

void Crowd::carPassengers(const Trains& trains, int ti, int k, bool ridden, float busy) {
  const Train& t = trains.trains()[static_cast<size_t>(ti)];
  const bool shink = trains.lines()[static_cast<size_t>(t.line)].kind == LineKind::Shinkansen;
  const Frame F = carFrame(trains, t, k);
  const bool end = k == 0 || k == t.cars - 1;
  const uint64_t base = carBase(t, k, trip_[t.id]);
  const CarLayout L = carLayout(shink, end);
  auto add = [&](double x, double y, double facing_model, int pose, uint64_t seed) {
    const V3 w = F.at(x, y, L.floor_z);  // (feet on the floor)
    CrowdPerson p;
    p.pos = w;
    p.yaw = F.yaw + static_cast<float>(facing_model);
    p.pose = pose;
    p.inside = true;
    style(p, seed, busy > 0.7f);
    people_.push_back(p);
  };
  const double occ = seatOccupancy(shink, busy);
  for (size_t i = 0; i < L.seats.size(); ++i) {
    if (ridden && static_cast<int>(i) == player_seat_) continue;  // the player's seat
    const uint64_t h = mix(base + static_cast<uint64_t>(i) + 1);
    if (u01(h) < occ) add(L.seats[i].x, L.seats[i].y, L.seats[i].facing, 2, h);
  }
  if (shink) return;
  // standing passengers holding the straps when it is busy (commuter cars)
  const double stand = std::clamp((busy - 0.55) / 0.45, 0.0, 1.0);
  if (stand <= 0.0) return;
  int i = 0;
  std::vector<float> cuts = {L.y0 + 0.4f};
  for (const auto& d : L.doors) cuts.insert(cuts.end(), {d.first - 0.15f, d.second + 0.15f});
  cuts.push_back(L.y1 - 0.4f);
  for (size_t c = 0; c + 1 < cuts.size(); c += 2) {
    const double a = cuts[c], b = std::min<double>(cuts[c + 1], L.y1 - 0.4);
    if (b - a < 0.6) continue;
    for (double sx : {-1.0, 1.0})
      for (double y = a + 0.2; y < b - 0.1; y += 0.64) {
        const uint64_t h = mix(base + static_cast<uint64_t>(++i) * 3);
        if (u01(h) < stand * 0.8) add(sx * 0.4, y, sx > 0 ? kPi / 2 : -kPi / 2, 3, h);  // facing the seats and windows
      }
  }
}

void Crowd::platformQueues(double now, const Trains& trains, int si, const V3& cam, float busy) {
  const Station& st = trains.stations()[static_cast<size_t>(si)];
  const auto kind = trains.lines()[static_cast<size_t>(st.line)].kind;
  const bool shink = kind == LineKind::Shinkansen;
  const int cars = kind == LineKind::Loop ? 10 : kind == LineKind::Branch ? 6 : 8;
  const double len = shink ? 25.0 : 20.0;
  const auto& L = trains.lines()[static_cast<size_t>(st.line)];
  for (int dir : {1, -1}) {  // trains of each direction stop at their own (left-hand) platform
    const int side_key = si * 2 + (dir > 0 ? 0 : 1);
    // the train standing at this platform, if any
    const Train* at = nullptr;
    for (const auto& t : trains.trains())
      if (t.at_station == si && t.dir == dir) at = &t;
    // door positions of a train stopped at the mark (a virtual one when none is there)
    Train v;
    v.line = st.line;
    v.dir = dir;
    v.cars = cars;
    v.car_len = len;
    v.s = st.s + dir * cars * len * 0.5;
    if (L.closed) {
      v.s = std::fmod(v.s, L.length);
      if (v.s < 0) v.s += L.length;
    }
    const double t_open = at ? (opened_.count(at->id) ? now - opened_[at->id] : 0.0) : -1.0;
    if (!at && !departed_.count(side_key)) departed_[side_key] = now - 600.0;  // queues already formed
    const double since_dep = at ? 0.0 : now - departed_[side_key];
    int door_i = 0;
    for (int k = 0; k < cars; ++k) {
      const Frame F = carFrame(trains, v, k);
      const bool reversed = k == cars - 1 && k > 0;
      // the platform is on the left of the direction of travel
      const double lsign = reversed ? 1.0 : -1.0;  // model x of the platform side
      std::vector<double> doors = shink ? std::vector<double>{-11.4} : std::vector<double>{-7.55, -2.52, 2.51, 7.54};
      for (double dy : doors) {
        const uint64_t h = mix(static_cast<uint64_t>(side_key) * 7777 + static_cast<uint64_t>(++door_i) * 31 + static_cast<uint64_t>(now / 600.0));
        const int want = static_cast<int>(u01(h) * (0.5 + busy * 4.5));  // people queueing at this door
        const V3 door = F.at(lsign * 1.45, dy, 0.0);
        if (std::hypot(door.x - cam.x, door.y - cam.y) > 90.0) continue;
        for (int j = 0; j < want; ++j) {
          bool show;
          if (at) show = t_open < 0.0 || Trains::doorOpen(*at) <= 0.0f || t_open < 1.4 + j * 1.1;  // boarding one by one
          else show = since_dep > 8.0 + j * 7.0;                                                   // the queue re-forms
          if (!show) continue;
          CrowdPerson p;
          // behind the tactile (yellow) line, 1.3 m back from the platform edge, one behind another
          const V3 w = F.at(lsign * (2.9 + 0.55 * j), dy + 0.1 * (static_cast<double>((h >> (j * 3)) % 3) - 1.0), 0.0);
          p.pos = {w.x, w.y, st.pos.z};
          p.yaw = F.yaw + static_cast<float>(lsign > 0 ? -kPi / 2 : kPi / 2);  // facing the track
          p.pose = 0;
          style(p, h + static_cast<uint64_t>(j), busy > 0.7f);
          people_.push_back(p);
        }
        // passengers getting off walk away along the platform towards the stairs (the centre)
        if (at && t_open >= 0.0 && Trains::doorOpen(*at) > 0.0f) {
          const int off = static_cast<int>(u01(mix(h + 99)) * (0.5 + busy * 3.0));
          for (int j = 0; j < off; ++j) {
            const double tt = t_open - 0.6 - j * 0.9;
            if (tt < 0.0 || tt > 14.0) continue;
            const double out = std::min(tt, 1.2) * 1.3;               // step out onto the platform
            const double along = std::max(0.0, tt - 1.2) * 1.3;       // then along it
            // towards the middle of the platform (the stairs): along the car's axis, away from the end it is nearer
            const V3 dp = F.at(0.0, dy, 0.0);
            const double mdir = ((dp.x - st.pos.x) * F.fx + (dp.y - st.pos.y) * F.fy) > 0.0 ? -1.0 : 1.0;
            CrowdPerson p;
            const V3 w = F.at(lsign * (1.6 + out), dy + mdir * along, 0.0);
            p.pos = {w.x, w.y, st.pos.z};
            p.yaw = F.yaw + static_cast<float>(tt < 1.2 ? (lsign > 0 ? kPi / 2 : -kPi / 2) : (mdir > 0 ? 0.0 : kPi));
            p.pose = 1;
            p.phase = static_cast<float>((out + along) / 0.7 * kPi);
            style(p, h * 3 + static_cast<uint64_t>(j), busy > 0.7f);
            people_.push_back(p);
          }
        }
      }
    }
    if (at) departed_[side_key] = now;  // (while a train stands here the queue is boarding)
  }
}

bool Crowd::jetSeatTaken(const Airliner& a, int seat) {
  const uint64_t h = mix(static_cast<uint64_t>(a.id) * 7121 + static_cast<uint64_t>(seat) + 1);
  return u01(h) <= 0.78;
}

void Crowd::jetCabin(const Airliner& a, int player_seat) {
  // seats as modelled (game/deck_layout.hpp), cabin floor at z = -0.72 m
  const Vector3 p = enuToRl(a.pos);
  const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a.roll), MatrixRotateX(a.pitch)), MatrixRotateY(-a.yaw)),
                                  MatrixTranslate(p.x, p.y, p.z));
  const auto seats = jetSeats();
  for (size_t i = 0; i < seats.size(); ++i) {
    if (static_cast<int>(i) == player_seat || !jetSeatTaken(a, static_cast<int>(i))) continue;
    const SeatSlot& s = seats[i];
    const Vector3 w = Vector3Transform(Vector3{s.x, kJetFloorZ, -(s.y - 0.03f)}, M);  // model (x, y, z) -> raylib (x, z, -y)
    CrowdPerson c;
    c.pos = rlToEnu(w);
    c.yaw = a.yaw;
    c.pitch = a.pitch;
    c.roll = a.roll;
    c.pose = 2;
    c.inside = true;
    style(c, mix(static_cast<uint64_t>(a.id) * 7121 + i + 1), false);
    people_.push_back(c);
  }
}

bool Crowd::ferrySeatTaken(const Ferry& f, int seat) {
  const uint64_t h = mix(static_cast<uint64_t>(f.id) * 9377 + static_cast<uint64_t>(seat) * 13 + 5);
  return u01(h) < 0.3;
}

void Crowd::ferryDeck(const Ferry& f, const Ferries& ferries, int player_seat) {
  const auto seats = ferrySeats(Ferries::shipClass(f.cls));
  const ShipClass& C = Ferries::shipClass(f.cls);
  for (size_t i = 0; i < seats.size(); ++i) {
    if (static_cast<int>(i) == player_seat || !ferrySeatTaken(f, static_cast<int>(i))) continue;
    CrowdPerson c;
    c.pos = ferries.toWorld(f, seats[i].x, seats[i].y, C.deck_z);
    c.yaw = f.yaw + seats[i].facing;
    c.pose = 2;
    style(c, mix(static_cast<uint64_t>(f.id) * 9377 + i * 13 + 5), false);
    people_.push_back(c);
  }
}

void Crowd::update(double now, const Trains& trains, const V3& cam, int ride_train, int ride_car, int hour, bool weekend, int player_seat) {
  player_seat_ = player_seat;
  busy_ = busyAt(hour, weekend);
  people_.clear();
  if (!trains.loaded()) return;
  const float busy = busyAt(hour, weekend);
  // bookkeeping: stops made (passengers change), when the doors opened
  for (const auto& t : trains.trains()) {
    const int prev = last_at_.count(t.id) ? last_at_[t.id] : -1;
    if (prev < 0 && t.at_station >= 0) {
      ++trip_[t.id];
      opened_[t.id] = now;
    }
    last_at_[t.id] = t.at_station;
  }
  const auto& all = trains.trains();
  for (size_t i = 0; i < all.size(); ++i) {
    const Train& t = all[i];
    if (t.offmap) continue;
    const bool open = Trains::doorOpen(t) > 0.0f;
    for (int k = 0; k < t.cars; ++k) {
      const bool ridden = t.id == ride_train && k == ride_car;
      const bool next_car = t.id == ride_train && std::abs(k - ride_car) == 1;  // (seen through the gangway)
      if (!ridden && !open && !next_car) continue;
      if (!ridden && !next_car) {
        V3 p;
        float yaw, pitch;
        trains.carPose(t, k, p, yaw, pitch);
        if (std::hypot(p.x - cam.x, p.y - cam.y) > 45.0) continue;
      }
      carPassengers(trains, static_cast<int>(i), k, ridden, busy);
    }
  }
  const auto& sts = trains.stations();
  for (size_t si = 0; si < sts.size(); ++si)
    if (std::hypot(sts[si].pos.x - cam.x, sts[si].pos.y - cam.y) < 200.0 && std::fabs(sts[si].pos.z - cam.z) < 40.0)
      platformQueues(now, trains, static_cast<int>(si), cam, busy);
}

}  // namespace rjc
