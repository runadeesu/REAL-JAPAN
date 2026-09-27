#pragma once
// Procedural train cars (no third-party assets): a generic stainless commuter car with a coloured
// band, and a generic white high-speed (Shinkansen-type) car with a long nose. Neither reproduces a
// real train series. Window openings are real holes in the shell (glass is a separate mesh) so the
// view out works when the player rides; the interior (floor, seats, lights) is drawn for that car.

#include "raylib.h"

namespace rjc {

// Seats the ride camera uses (car model space: x right, y forward, z up from the rail top).
// Shinkansen: left window seat of the row nearest the car centre, facing forward.
// Commuter: long bench seat on the left, facing across the car.
constexpr float kTrainFloorAbove = 0.05f;  // interior floor above the side sill (zFloor)
constexpr float kCommuterFloorZ = 1.1f + kTrainFloorAbove;
constexpr float kShinkansenFloorZ = 1.1f + kTrainFloorAbove;
constexpr float kShinkansenSeatRow0 = -12.5f + 1.5f, kShinkansenSeatPitch = 1.04f;

struct TrainCarModel {
  Mesh shell{};
  Mesh glass{};
  Mesh interior{};
  float length = 20.0f;
};

enum class TrainCar : int { CommuterMid = 0, CommuterCab, ShinkansenMid, ShinkansenNose, Count };

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
