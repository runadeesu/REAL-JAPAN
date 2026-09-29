#pragma once
// Where one can walk and sit aboard the ferries and the airliner (shared by the models in
// render/ships.cpp and render/aircraft.cpp, the passengers and the player, app_transit.cpp).
//  * ferry, ship frame (x right, y forward, z up from the waterline): the open passenger deck
//    round the deckhouse, benches in rows on the open deck aft of it (facing aft, over the wake)
//  * airliner, model frame (x right, y forward, z up from the fuselage axis): the cabin aisle and
//    the forward vestibule by the front left door, 2 + 2 seats in rows

#include <cmath>
#include <vector>

#include "game/car_layout.hpp"
#include "game/ferries.hpp"

namespace rjc {

// --- ferry ---
struct BenchRow {
  float y;          // seat line (hips)
  float x0, x1;     // each half of the row runs from x0 to x1 (and from -x1 to -x0)
};

inline std::vector<BenchRow> ferryBenchRows(const ShipClass& C) {
  std::vector<BenchRow> rows;
  for (float y = C.deck_y0 + 2.5f; y < C.house_y0 - 1.5f; y += 2.2f) rows.push_back({y, 0.6f, C.deck_x - 1.2f});
  return rows;
}

inline std::vector<SeatSlot> ferrySeats(const ShipClass& C) {
  constexpr float kPi = 3.14159265358979f;
  std::vector<SeatSlot> s;
  for (const auto& r : ferryBenchRows(C))
    for (float sx : {-1.0f, 1.0f})
      for (float x = r.x0 + 0.275f; x <= r.x1 - 0.2f; x += 0.55f) s.push_back({sx * x, r.y, kPi, sx * x, r.y - 0.65f});
  return s;
}

// Can one stand at (x, y) on the open deck? (not in the deckhouse, not on a bench)
inline bool ferryDeckWalkable(const ShipClass& C, double x, double y, double r = 0.3) {
  if (std::fabs(x) > C.deck_x - r + 0.35 || y < C.deck_y0 + r || y > C.deck_y1 - r) return false;
  if (std::fabs(x) < C.house_x + r && y > C.house_y0 - r && y < C.house_y1 + r) return false;
  for (const auto& b : ferryBenchRows(C))
    if (y > b.y - 0.3 - r * 0.5 && y < b.y + 0.32 + r * 0.5 && std::fabs(x) > b.x0 - r * 0.5 && std::fabs(x) < b.x1 + 0.25 + r * 0.5) return false;
  return true;
}

// --- airliner ---
constexpr float kJetFloorZ = -0.72f;                 // cabin floor
constexpr float kJetCabinY0 = -9.8f, kJetCabinY1 = 10.8f;
constexpr float kJetDoorY0 = 11.5f, kJetDoorY1 = 12.5f;  // front left door (opening in the fuselage)
constexpr float kJetVestibuleY1 = 12.9f;
constexpr float kJetSkinX = 1.5f;
constexpr float kJetStairRun = 2.3f;                 // the passenger stairs' run (out from the landing)
constexpr float kJetStairFootX = -kJetSkinX - 0.05f - 0.8f - kJetStairRun - 0.3f;  // where one steps on / off (apron)

inline std::vector<SeatSlot> jetSeats() {
  std::vector<SeatSlot> s;
  for (float ry = kJetCabinY0 + 0.6f; ry < kJetCabinY1 - 0.6f; ry += 0.8f)
    for (float x : {-1.0f, -0.55f, 0.55f, 1.0f}) s.push_back({x, ry - 0.02f, 0.0f, 0.0f, ry - 0.02f});
  return s;
}

// In the cabin: the aisle between the seats, the vestibule, and out through the open door.
inline bool jetCabinWalkable(double x, double y, bool door_open, double r = 0.25) {
  if (y > kJetCabinY1 - 0.1 && y < kJetVestibuleY1 - r) {  // vestibule (galley) by the door
    if (x < -(1.3 - r)) return door_open && y > kJetDoorY0 + 0.1 && y < kJetDoorY1 - 0.1 && x > -(kJetSkinX + 0.9);
    return x < 1.3 - r;
  }
  if (y < kJetCabinY0 + r || y > kJetVestibuleY1 - r) return false;
  return std::fabs(x) < 0.34 - r * 0.4;  // the aisle
}

}  // namespace rjc
