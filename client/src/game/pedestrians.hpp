#pragma once
// People on foot, drawn in 3D. Two populations share routing and rendering:
//
//  * Residents: who walks, from where to where, and when comes from each resident's daily plan
//    (rjcore schedules; fictional people living in real buildings). When a walker first appears
//    its position along the route is the one implied by the schedule.
//  * Visitors (来街者): the crowd of a commercial district is mostly people who do not live there.
//    They are generated STATISTICALLY — a game assumption, not measured people-flow data: an
//    hourly profile scaled by the floor area of the real buildings around the player (PLATEAU
//    usage codes: commercial, business, hotels, shop-houses) and by the real station entrances.
//    Each visitor walks from a real entrance / building door to another.
//
// Routes use A* on a walkability grid built from real building footprints, with traversal costs:
// sidewalks and crossings are cheap, carriageways dear (narrow shared streets only slightly), so
// people keep to the sidewalks and cross at the crossings. At signalised crossings they wait for
// the pedestrian green. Everyone walks at a real pace (about 1.4 m/s) regardless of game time.

#include <cstdint>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "raylib.h"
#include "game/town_sim.hpp"
#include "rj/geo/local_frame.hpp"
#include "rj/nav/grid_nav.hpp"
#include "rj/sim/calendar.hpp"

namespace rjc {

class World;
class Traffic;
class TrafficSignals;
class RoadMarkings;

struct Walker {
  size_t npc = 0;
  bool visitor = false;  // statistical visitor (no resident record)
  WalkTrip trip;
  std::vector<rj::nav::Vec2> path;
  double length = 0;
  double dist = 0;
  rj::nav::Vec2 pos, dir{0, 1};
  float z = 0;
  float yaw = 0;  // compass radians, smoothed through route corners
  float phase = 0;
  float speed = 1.4f;                         // m/s
  float offset = 0, off_eff = 0;              // lateral offset from the route centre line (m)
  float wait_back = 0.8f;                     // how far before a crossing they stop on red
  std::vector<std::pair<float, int>> xings;   // (route distance entering a crossing, crossing id)
  size_t next_x = 0;
  Color shirt{}, pants{}, skin{}, hair{};
  int variant = 0;        // body variant (appearance only): trousers / long hair / skirt
  int age = 30;
  bool waiting = false;   // standing at a red pedestrian signal
  float height_scale = 1.0f;
  rj::nav::Vec2 dodge{0, 0};  // stepped aside from the player's car (decays back to the route)
};

class Pedestrians {
 public:
  ~Pedestrians();
  // traffic / markings may be null (no road graph): then every free cell costs the same.
  void buildNav(const World& world, const Traffic* traffic = nullptr, const RoadMarkings* markings = nullptr);
  bool navReady() const { return nav_.valid(); }
  void clear();

  // The player's car: people ahead of it stop and step aside; nobody is driven through.
  void setHazard(bool on, const rj::geo::Vec3d& p, double yaw, double speed) {
    hz_on_ = on;
    hz_ = p;
    hz_yaw_ = yaw;
    hz_v_ = speed;
  }
  void update(TownSim& town, const World& world, const TrafficSignals& signals, const rj::sim::CivilDateTime& now,
              const rj::geo::Vec3d& player, float real_dt, float crowd_factor = 1.0f);
  const std::map<size_t, Walker>& walkers() const { return walkers_; }
  const Walker* pick(const rj::geo::Vec3d& eye, const rj::geo::Vec3d& dir, double max_dist) const;
  size_t visitorCount() const { return n_visitors_; }
  size_t sourceCount() const { return sources_.size(); }
  float activity() const { return activity_; }

  static constexpr double kVisibleRadius = 450.0;
  static constexpr size_t kMaxWalkers = 140;    // residents
  static constexpr size_t kMaxVisitors = 520;
  static constexpr size_t kVisitorIdBase = size_t{1} << 40;

 private:
  struct Job {
    size_t npc;
    WalkTrip trip;
    rj::nav::Vec2 from, to;
    bool visitor = false;
    bool mid_route = false;  // initial fill: start somewhere along the route
  };
  struct Result {
    Job job;
    std::optional<std::vector<rj::nav::Vec2>> path;
  };
  struct Source {
    rj::nav::Vec2 door;
    float weight;
    bool station;
  };
  void startJobs();
  void buildSources(const World& world);
  void spawnVisitors(const rj::nav::Vec2& me, int hour, float crowd_factor, float real_dt);
  void markCrossings(Walker& w) const;

  rj::nav::GridNav nav_;
  std::vector<int16_t> cross_id_;  // per nav cell: crossing index or -1
  const RoadMarkings* markings_ = nullptr;
  std::map<size_t, Walker> walkers_;
  std::map<size_t, int> pending_;  // npc -> trip start minute being routed
  std::map<size_t, int> failed_;   // npc -> trip start minute that had no route
  std::vector<Job> queue_;
  std::future<std::vector<Result>> job_;
  int last_minute_ = -1;
  rj::sim::CivilDate last_date_{0, 0, 0};
  std::vector<WalkTrip> trips_;
  // visitors
  std::vector<Source> sources_;
  std::vector<int> near_src_;        // sources within reach of the player
  std::vector<float> near_cum_;      // cumulative weights of near_src_
  rj::nav::Vec2 near_at_{1e30, 1e30};
  float activity_ = 0.0f;
  float spawn_acc_ = 0.0f;
  size_t n_visitors_ = 0, visitors_pending_ = 0;
  size_t next_visitor_ = kVisitorIdBase;
  bool filled_ = false;
  uint64_t rng_ = 0x9e3779b97f4a7c15ULL;
  double rnd();
  bool hz_on_ = false;
  rj::geo::Vec3d hz_{};
  double hz_yaw_ = 0, hz_v_ = 0;
};

}  // namespace rjc
