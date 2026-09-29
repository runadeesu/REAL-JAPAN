#pragma once
// Interior layout of the generic train cars (as modelled in render/trains.cpp): where the seats are,
// where one can stand and walk, and where the doors and gangways are. Shared by the passengers
// (crowd.cpp), the player walking about the car (app_transit.cpp) and the renderer. Car model
// frame: x to the right, y forward, z up from the rail top; the rear cab car is drawn turned round,
// so its model frame is too.

#include <cmath>
#include <utility>
#include <vector>

#include "game/trains.hpp"

namespace rjc {

struct SeatSlot {
  float x, y;         // hip position on the seat (model frame)
  float facing;       // model yaw of the seated person (0 = forward, +pi/2 = to the right)
  float fx, fy;       // where to stand to sit down / after standing up
};

// Shinkansen seat rows (2 + 3 reclining seats facing forward) begin behind the entrance vestibule.
constexpr float kShinkansenRow0 = -12.5f + 2.4f, kShinkansenRowPitch = 1.04f;
constexpr float kGangwayHalfW = 0.45f;  // the gangway door opening in the car ends
constexpr float kGangwayTop = 1.95f;    // its head above the floor

struct CarLayout {
  bool shink = false;
  float half_w = 1.45f;      // inner half width of the body
  float y0 = -10.0f, y1 = 10.0f;  // walkable length (a cab takes the front of end cars)
  float floor_z = 1.15f;     // floor above the rail top
  std::vector<std::pair<float, float>> doors;  // door spans along y (both sides)
  std::vector<SeatSlot> seats;
  bool gangway_front = true, gangway_rear = true;  // a gangway to the next car at that end
};

inline CarLayout carLayout(bool shinkansen, bool cab_end) {
  CarLayout L;
  L.shink = shinkansen;
  constexpr float kPi = 3.14159265358979f;
  if (shinkansen) {
    L.half_w = 1.69f;
    L.y0 = -12.5f;
    L.y1 = cab_end ? 1.0f - 0.3f : 12.5f;  // (the nose car: up to the partition in front of the cab)
    L.gangway_front = !cab_end;
    L.doors = {{-12.5f + 0.6f, -12.5f + 1.6f}};
    // rows of 2 + 3 forward-facing seats 1.04 m apart; aisle between x = -0.69 and x = 0.14
    const float body_y1 = cab_end ? 1.0f : 12.5f;  // (as modelled)
    for (float y = kShinkansenRow0; y < body_y1 - 1.0f; y += kShinkansenRowPitch)
      for (float x : {-1.35f, -0.9f, 0.35f, 0.82f, 1.29f}) L.seats.push_back({x, y - 0.08f, 0.0f, -0.28f, y - 0.08f});
    return L;
  }
  L.half_w = 1.45f;
  L.y0 = -10.0f;
  L.y1 = cab_end ? 10.0f - 1.6f : 10.0f;
  L.gangway_front = !cab_end;
  for (int d = 0; d < 4; ++d) {
    const float yc = -10.0f + 2.45f + d * 5.03f;
    L.doors.push_back({yc - 0.65f, yc + 0.65f});
  }
  // long bench seats between the doors, on both sides, facing across the car
  std::vector<float> cuts = {L.y0 + 0.4f};
  for (const auto& d : L.doors) cuts.insert(cuts.end(), {d.first - 0.15f, d.second + 0.15f});
  cuts.push_back(L.y1 - 0.4f);
  for (size_t c = 0; c + 1 < cuts.size(); c += 2) {
    const float a = cuts[c], b = std::fmin(cuts[c + 1], L.y1 - 0.4f);
    if (b - a < 0.6f) continue;
    const int n = static_cast<int>((b - a) / 0.46f);
    for (float sx : {-1.0f, 1.0f})
      for (int j = 0; j < n; ++j) {
        const float y = a + (b - a) * (j + 0.5f) / n;
        L.seats.push_back({sx * 1.05f, y, sx > 0 ? -kPi / 2 : kPi / 2, sx * 0.55f, y});
      }
  }
  return L;
}

// Is (x, y) a place a standing person can occupy (model frame)? Open doors let one out onto the
// platform (up to 0.6 m beyond the side), the gangways into the next car (0.6 m beyond the end).
inline bool carWalkable(const CarLayout& L, float x, float y, bool door_open_side_left, bool door_open_side_right, float r = 0.25f) {
  if (y > L.y1 - r) return L.gangway_front && y < L.y1 + 0.6f && std::fabs(x) < kGangwayHalfW - r * 0.5f;
  if (y < L.y0 + r) return L.gangway_rear && y > L.y0 - 0.6f && std::fabs(x) < kGangwayHalfW - r * 0.5f;
  auto inDoor = [&](float yy) {
    for (const auto& d : L.doors)
      if (yy > d.first + 0.05f && yy < d.second - 0.05f) return true;
    return false;
  };
  const bool door = inDoor(y);
  if (L.shink) {
    const bool vestibule = y < kShinkansenRow0 - 0.55f - r;  // the entry space by the door, behind its partition
    if (vestibule) {
      const float lim = L.half_w - r;
      if (x < -lim) return door && door_open_side_left && x > -(L.half_w + 0.6f);
      if (x > lim) return door && door_open_side_right && x < L.half_w + 0.6f;
      return true;
    }
    return x > -0.69f + r * 0.6f && x < 0.14f - r * 0.6f;  // the aisle
  }
  // commuter: the space between the long seats; at the doors up to the door threshold
  const float free_half = door ? L.half_w - r : 0.62f;
  if (x < -free_half) return door && door_open_side_left && x > -(L.half_w + 0.6f);
  if (x > free_half) return door && door_open_side_right && x < L.half_w + 0.6f;
  return true;
}

// A car's pose as drawn: world = p + r * x + f * y + up * (z + tanp * y) (origin ENU).
struct CarFrame {
  rj::geo::Vec3d p;
  double fx = 0, fy = 1, rx = 1, ry = 0, tanp = 0;
  float yaw = 0, pitch = 0;
  bool reversed = false;  // the rear cab car, turned round
  rj::geo::Vec3d at(double x, double y, double z) const { return {p.x + rx * x + fx * y, p.y + ry * x + fy * y, p.z + z + tanp * y}; }
  void local(const rj::geo::Vec3d& w, double& x, double& y) const {
    const double dx = w.x - p.x, dy = w.y - p.y;
    x = dx * rx + dy * ry;
    y = dx * fx + dy * fy;
  }
};

inline bool carReversed(const Train& t, int k) { return k == t.cars - 1 && k > 0; }

inline CarFrame carFrame(const Trains& trains, const Train& t, int k) {
  CarFrame F;
  float yaw, pitch;
  trains.carPose(t, k, F.p, yaw, pitch);
  F.reversed = carReversed(t, k);
  if (F.reversed) {
    yaw += 3.14159265358979f;
    pitch = -pitch;
  }
  F.yaw = yaw;
  F.pitch = pitch;
  F.fx = std::sin(yaw);
  F.fy = std::cos(yaw);
  F.rx = F.fy;
  F.ry = -F.fx;
  F.tanp = std::tan(pitch);
  return F;
}

inline CarLayout carLayoutOf(const Trains& trains, const Train& t, int k) {
  return carLayout(trains.lines()[static_cast<size_t>(t.line)].kind == LineKind::Shinkansen, k == 0 || k == t.cars - 1);
}

// The platform is on the left of the direction of travel: the model's left side (-x), or its
// right for the rear cab car, which is turned round.
inline bool doorSideLeft(const Train& t, int k) { return !carReversed(t, k); }

}  // namespace rjc
