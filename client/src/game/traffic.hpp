#pragma once
// Road traffic on the real carriageway graph (RJROAD, derived from PLATEAU tran LOD2).
//
//  * left-hand traffic; lanes per direction from the measured carriageway width
//    (lane counts / one-way rules are not in the data: every road is two-way)
//  * IDM car-following, speed by road class, yielding at unsignalised junctions
//  * obeys the real traffic signals (PLATEAU frn 4900 heads; timing is a game assumption)
//  * Traffic Simulation LOD: vehicles exist only within ~380 m of the player; density follows
//    the time of day (rush hours, night)
//  * vehicle mix typical of central Tokyo: sedans, taxis, kei cars, minivans, delivery vans,
//    2 t trucks, buses (wide roads only)

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;
class TrafficSignals;

enum class VehicleType : int { Sedan = 0, Taxi, Kei, Minivan, Van, Truck, Bus, Count };

struct Vehicle {
  int id = 0;
  VehicleType type = VehicleType::Sedan;
  int edge = -1;
  int dir = 0;    // 0: a -> b, 1: b -> a
  int lane = 0;   // 0 = leftmost (kerb side)
  double s = 0;   // metres along the edge in travel direction
  double v = 0;   // m/s
  double acc = 0;
  int next_edge = -1, next_dir = 0;
  bool braking = false;
  float color[3] = {1, 1, 1};
  // render state (origin ENU)
  rj::geo::Vec3d pos;
  float yaw = 0;  // compass radians
  float pitch = 0;
  float roll = 0;        // body roll (positive: right side down)
  float steer = 0;       // front road-wheel angle (positive: right)
  float wheel_dist = 0;  // distance rolled (wheel rotation)
  bool reversing = false;
  int blink = 0;          // turn indicator: -1 left, 1 right (before and through a turn at a junction)
  float blink_t = 0;      // keeps blinking a moment after the turn
};

class Traffic {
 public:
  bool load(const std::filesystem::path& file, std::string& err);
  void place(const World& world);  // (re)compute origin ENU geometry (after load / origin rebase)
  void update(double dt, const World& world, const TrafficSignals& signals, const rj::geo::Vec3d& player, int hour);
  const std::vector<Vehicle>& vehicles() const { return veh_; }
  bool loaded() const { return !edges_.empty(); }
  size_t edgeCount() const { return edges_.size(); }
  double networkKm() const;
  static float lengthOf(VehicleType t);

  struct Edge {
    int a, b;
    float width;
    std::vector<rj::geo::Geodetic> geo;
    std::vector<rj::geo::Vec3d> pts;  // origin ENU (z = terrain)
    std::vector<double> cum;          // cumulative length
    double length = 0;
    int lanes = 1;                    // per direction
    double lane_w = 3.0;
    double v0 = 11.0;
    float stop[2] = {-1.0f, -1.0f};   // stop-line distance from node a / node b (-1: default)
  };
  struct Node {
    rj::geo::Geodetic geo;
    rj::geo::Vec3d pos;
    std::vector<int> edges;
    int signal_group = -1;  // nearest real signal group (vehicle heads within 30 m)
    int est_group = -1;     // estimated signal group (RoadMarkings) when no real heads are near
  };
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<Edge>& edges() const { return edges_; }
  // Signal groups are assigned lazily in update(); markings need them up front.
  void assignSignalGroups(const TrafficSignals& signals) { assignSignals(signals); }
  void setStopDistance(int edge, bool at_b, float d) { edges_[static_cast<size_t>(edge)].stop[at_b ? 1 : 0] = d; }
  void setEstimatedGroup(int node, int group) { nodes_[static_cast<size_t>(node)].est_group = group; }
  void clearRoadMarkingState();  // stop distances and estimated groups (before markings are rebuilt)
  // The player's car: an obstacle the AI vehicles keep their distance from.
  void setObstacle(bool on, const rj::geo::Vec3d& p) {
    obstacle_on_ = on;
    obstacle_ = p;
  }
  // Remove vehicle `id` from the simulation (the player takes the wheel); false if gone.
  bool take(int id, Vehicle& out);
  // Kerb-side lane pose nearest to p, travelling as close to `yaw_hint` as the road allows.
  bool nearestLane(const rj::geo::Vec3d& p, double yaw_hint, rj::geo::Vec3d& out, double& heading) const;

 private:
  void samplePose(const Edge& e, int dir, int lane, double s, rj::geo::Vec3d& p, double& heading) const;
  double headingAtEnd(const Edge& e, int dir) const;
  double headingAtStart(const Edge& e, int dir) const;
  void chooseNext(Vehicle& v);
  bool spawnOne(const rj::geo::Vec3d& player, double rmin, double rmax);
  void assignSignals(const TrafficSignals& signals);

  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<Vehicle> veh_;
  int next_id_ = 1;
  uint32_t rng_ = 1234567u;
  double signal_check_t_ = 0;
  std::vector<int> cand_;
  std::vector<float> cand_w_;
  rj::geo::Vec3d cand_at_{1e30, 1e30, 0};
  int warm_frames_ = 0;
  bool obstacle_on_ = false;
  rj::geo::Vec3d obstacle_{};
  float rnd();
};

}  // namespace rjc
