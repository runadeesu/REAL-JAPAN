#pragma once
// Railway passengers near the camera (procedural people; how full the trains and platforms are is
// a game assumption by time of day, not ridership data):
//  * in the train cars the player can see into (the car ridden, and cars standing at a platform
//    with their doors open): people on the seats and, when it is busy, standing holding straps;
//    they change at each stop
//  * on platforms: short queues at the door positions (as marked on Japanese platforms), who
//    board one by one when the doors open and re-form after the train has gone; passengers who
//    got off walk away along the platform

#include <cstdint>
#include <map>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class Trains;

struct CrowdPerson {
  rj::geo::Vec3d pos;  // feet (origin ENU)
  float yaw = 0;       // compass radians (facing)
  int pose = 0;        // 0 standing, 1 walking, 2 seated, 3 holding a strap
  float phase = 0;     // walk cycle
  int variant = 0;     // body variant
  Color top{}, bottom{}, skin{}, hair{};
  float scale = 1;
  bool inside = false;  // in a train car (lit by the car's lights)
};

class Crowd {
 public:
  // cam: camera position; ride_train / ride_car: the car the player sits in (-1: none); eye: the
  // player's eye there (their own seat stays free); hour: local time; weekend: fewer commuters
  void update(double now, const Trains& trains, const rj::geo::Vec3d& cam, int ride_train, int ride_car, const rj::geo::Vec3d& eye, int hour,
              bool weekend);
  const std::vector<CrowdPerson>& people() const { return people_; }

 private:
  void carPassengers(const Trains& trains, int train_index, int k, bool ridden, const rj::geo::Vec3d& eye, float busy);
  void platformQueues(double now, const Trains& trains, int station, const rj::geo::Vec3d& cam, float busy);
  std::vector<CrowdPerson> people_;
  std::map<int, int> trip_;            // train id -> stops made (the passengers change at each)
  std::map<int, int> last_at_;         // train id -> station it stood at last update
  std::map<int, double> opened_;       // train id -> when its doors opened at the current stop
  std::map<int, double> departed_;     // station * 2 + side -> when the last train left that platform
};

}  // namespace rjc
