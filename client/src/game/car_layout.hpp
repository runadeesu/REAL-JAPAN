#pragma once
// Interior layout of the generic train cars (as modelled in render/trains.cpp): where the seats are,
// where one can stand and walk, and where the doors are. Shared by the passengers (crowd.cpp), the
// player walking about the car and the renderer. Car model frame: x to the right, y forward, z up
// from the rail top; the rear cab car is drawn turned round, so its model frame is too.

#include <cmath>
#include <utility>
#include <vector>

namespace rjc {

struct SeatSlot {
  float x, y;         // hip position on the seat (model frame)
  float facing;       // model yaw of the seated person (0 = forward, +pi/2 = to the right)
  float fx, fy;       // where to stand to sit down / after standing up
};

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
    L.y1 = cab_end ? 1.0f : 12.5f;
    L.gangway_front = !cab_end;
    L.doors = {{-12.5f + 0.6f, -12.5f + 1.6f}};
    // rows of 2 + 3 forward-facing seats 1.04 m apart; aisle between x = -0.62 and x = 0.07
    for (float y = L.y0 + 1.5f; y < L.y1 - 1.0f; y += 1.04f)
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

// Is (x, y) a place a standing person can occupy (model frame)?
inline bool carWalkable(const CarLayout& L, float x, float y, bool door_open_side_left, bool door_open_side_right, float r = 0.25f) {
  if (y < L.y0 + r || y > L.y1 - r) return false;
  auto inDoor = [&](float yy) {
    for (const auto& d : L.doors)
      if (yy > d.first + 0.05f && yy < d.second - 0.05f) return true;
    return false;
  };
  const bool door = inDoor(y);
  if (L.shink) {
    const bool vestibule = y < L.doors[0].second + 0.6f;  // the entry space by the door at the rear of the car
    if (vestibule) {
      const float lim = L.half_w - r;
      if (x < -lim) return door && door_open_side_left && x > -(L.half_w + 0.6f);
      if (x > lim) return door && door_open_side_right && x < L.half_w + 0.6f;
      return true;
    }
    return x > -0.62f + r * 0.5f && x < 0.07f - r * 0.5f;  // the aisle
  }
  // commuter: the space between the long seats; at the doors up to the door threshold
  const float free_half = door ? L.half_w - r : 0.62f;
  if (x < -free_half) return door && door_open_side_left && x > -(L.half_w + 0.6f);
  if (x > free_half) return door && door_open_side_right && x < L.half_w + 0.6f;
  return true;
}

}  // namespace rjc
