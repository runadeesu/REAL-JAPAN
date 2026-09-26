#pragma once
// Residents on foot, drawn in 3D.
//
// Who walks, from where to where, and when comes from each resident's daily
// plan (rjcore schedules). The route is found with A* on a walkability grid
// built from the real building footprints. When a walker first appears its
// position along the route is the one implied by the schedule; after that it
// walks at a real pace (1.4 m/s) so that game-time compression does not make
// people sprint. These are the simulated residents, not random spawns.

#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "raylib.h"
#include "game/town_sim.hpp"
#include "rj/geo/local_frame.hpp"
#include "rj/nav/grid_nav.hpp"
#include "rj/sim/calendar.hpp"

namespace rjc {

class World;

struct Walker {
  size_t npc = 0;
  WalkTrip trip;
  std::vector<rj::nav::Vec2> path;
  double length = 0;
  double dist = 0;
  rj::nav::Vec2 pos, dir{0, 1};
  float z = 0;
  float phase = 0;
  Color shirt{}, pants{}, skin{};
  float height_scale = 1.0f;
};

class Pedestrians {
 public:
  ~Pedestrians();
  void buildNav(const World& world);
  bool navReady() const { return nav_.valid(); }
  void clear();

  void update(TownSim& town, const World& world, const rj::sim::CivilDateTime& now,
              const rj::geo::Vec3d& player, float real_dt);
  const std::map<size_t, Walker>& walkers() const { return walkers_; }
  const Walker* pick(const rj::geo::Vec3d& eye, const rj::geo::Vec3d& dir, double max_dist) const;

  static constexpr double kVisibleRadius = 450.0;
  static constexpr size_t kMaxWalkers = 140;

 private:
  struct Job {
    size_t npc;
    WalkTrip trip;
    rj::nav::Vec2 from, to;
  };
  struct Result {
    size_t npc;
    WalkTrip trip;
    std::optional<std::vector<rj::nav::Vec2>> path;
  };
  void startJobs();

  rj::nav::GridNav nav_;
  std::map<size_t, Walker> walkers_;
  std::map<size_t, int> pending_;  // npc -> trip start minute being routed
  std::map<size_t, int> failed_;   // npc -> trip start minute that had no route
  std::vector<Job> queue_;
  std::future<std::vector<Result>> job_;
  int last_minute_ = -1;
  rj::sim::CivilDate last_date_{0, 0, 0};
  std::vector<WalkTrip> trips_;
};

}  // namespace rjc
