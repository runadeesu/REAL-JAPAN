#pragma once
// Procedural train cars (no third-party assets): a generic stainless commuter car with a coloured
// band, and a generic white high-speed (Shinkansen-type) car with a long nose. Neither reproduces a
// real train series. Window openings are real holes in the shell (glass is a separate mesh) so the
// view out works when the player rides; the interior (floor, seats, lights) is drawn for that car.

#include "raylib.h"

namespace rjc {

// Car model space: x right, y forward, z up from the rail top. Seats, doors and gangways as the
// passengers and the player use them: game/car_layout.hpp.
constexpr float kTrainFloorAbove = 0.05f;  // interior floor above the side sill (zFloor)
constexpr float kCommuterFloorZ = 1.1f + kTrainFloorAbove;
constexpr float kShinkansenFloorZ = 1.1f + kTrainFloorAbove;

struct TrainCarModel {
  Mesh shell{};
  Mesh glass{};
  Mesh interior{};
  // sliding door leaves, [side: 0 left (-x), 1 right (+x)][slides towards: 0 -y, 1 +y], drawn closed
  // at the origin and moved by up to door_travel along y when the doors open
  Mesh doors[2][2]{};
  float door_travel = 0.64f;
  float length = 20.0f;
};

enum class TrainCar : int { CommuterMid = 0, CommuterCab, ShinkansenMid, ShinkansenNose, ShinkansenPanto, CommuterPanto, Count };

class TrainModels {
 public:
  void build();
  void unload();
  bool ready() const { return ready_; }
  const TrainCarModel& get(TrainCar c) const { return m_[static_cast<int>(c)]; }

 private:
  TrainCarModel m_[static_cast<int>(TrainCar::Count)];
  bool ready_ = false;
};

}  // namespace rjc
