#pragma once
// Traffic signals at their real positions (PLATEAU frn 4900 signal heads, orientation and
// intersection grouping from the pipeline). The timing is a game-side assumption — real signal
// plans are not public data: each intersection runs two orthogonal phases on a 100 s cycle with
// yellow and all-red clearance; pedestrian heads show walk / flashing / stop in step with the
// parallel vehicle phase. Runs in real time (signals do not speed up with the game clock).

#include <cstdint>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

enum class VehLamp : int { Green = 0, Yellow = 1, Red = 2 };
enum class PedLamp : int { Walk = 0, Flash = 1, Stop = 2 };

class TrafficSignals {
 public:
  struct Head {
    rj::geo::Vec3d pos;  // origin ENU
    float facing;        // compass radians (direction the lamps face)
    float length;
    int kind;            // 0 vehicle, 1 pedestrian
    int group;           // global intersection id
    int phase;           // 0 / 1
  };
  void rebuild(const World& world);
  void update(double real_time_s) { t_ = real_time_s; }
  const std::vector<Head>& heads() const { return heads_; }
  VehLamp vehicle(int group, int phase) const;
  PedLamp pedestrian(int group, int phase) const;
  bool flashOn() const;  // blink state for flashing pedestrian green
  int groups() const { return ngroups_; }

  static constexpr double kCycle = 100.0;

 private:
  double local(int group) const;
  std::vector<Head> heads_;
  int ngroups_ = 0;
  double t_ = 0.0;
};

}  // namespace rjc
