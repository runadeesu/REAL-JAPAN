#pragma once
// Ferries of the fictional country (data/world/country/transport.txt): a short-route ferry between
// the capital's harbour and the islet, a high-speed ferry from the capital to the southern island,
// and a car ferry from the port city to the southern island. Fictional services with no published
// timetable.
//
// Harbour manoeuvres: each trip starts by backing away from the berth, turning on the spot (bow
// thrusters) and then following the route at cruising speed; ships slow down in the harbour and
// come alongside the pier bow first. Ships heave, pitch and roll on the swell.
// The player boards at the pier, walks about the open deck while under way, and leaves the ship
// once it is alongside again.

#include <filesystem>
#include <string>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

struct Pier {
  rj::geo::Geodetic geo;  // root of the pier at the coast
  double heading = 0;     // degrees, pointing out to sea
  double length = 120.0;
  rj::geo::Vec3d pos;     // origin ENU (deck height is 2.4 m above sea level)
  std::string name;       // e.g. 島ノ浦港
  int ships = 0;          // ships berthing here (each takes one side of the pier)
};

struct ShipClass {
  float length, beam, freeboard;  // m
  float deck_z;                   // open (walkable) passenger deck above the waterline
  float deck_x, deck_y0, deck_y1; // walkable half width and fore/aft extent (ship frame: x right, y forward)
  float house_x, house_y0, house_y1;  // deckhouse on that deck (not walkable)
};

struct Ferry {
  int id = 0;
  int route = 0;
  int cls = 0;              // 0 small (62 m), 1 large car ferry (110 m)
  double cruise = 8.5;      // service speed (m/s): high-speed ferries run much faster
  int side_a = 1, side_b = -1;  // which side of each pier it berths at
  // path in origin ENU; trip goes from path[0] to path.back() (dir +1) or back (dir -1)
  std::vector<rj::geo::Vec3d> path;
  std::vector<double> cum;
  double length = 0;
  int pier_a = -1, pier_b = -1;  // pier_b < 0: the far end is off the map
  double s = 0, v = 0;           // along the path, signed speed along the trip direction
  int dir = 1;
  enum class Phase { Docked, Astern, Turning, Under, Offmap } phase = Phase::Docked;
  double timer = 0;              // dwell / off-map wait
  double astern_len = 0;         // length of the backing leg at the start of each trip
  // pose (origin ENU): centre at the waterline
  rj::geo::Vec3d pos;
  float yaw = 0, pitch = 0, roll = 0, heave = 0;
  float wake = 0;                // 0..1 how much wake the hull makes
  std::vector<rj::geo::Vec3d> trail;  // recent stern positions (for the wake)
};

class Ferries {
 public:
  bool load(const std::filesystem::path& transport, std::string& err);
  bool loaded() const { return !routes_.empty() && !piers_.empty(); }
  void place(const World& world);
  void update(double dt, double t_s, float wind);
  const std::vector<Ferry>& ships() const { return ships_; }
  const std::vector<Pier>& piers() const { return piers_; }
  const Ferry* ship(int id) const;
  static const ShipClass& shipClass(int cls);
  // the ship docked at pier `p` (doors open), or nullptr
  const Ferry* dockedAt(int pier) const;
  int pierNear(const rj::geo::Vec3d& p, double r) const;  // on or near a pier deck
  // where a docked ship's gangway lands on the pier / on board (origin ENU)
  rj::geo::Vec3d gangwayPier(const Ferry& f) const;
  // ship frame (x right, y forward, z up from the waterline) <-> origin ENU
  rj::geo::Vec3d toWorld(const Ferry& f, double x, double y, double z) const;
  void toShip(const Ferry& f, const rj::geo::Vec3d& p, double& x, double& y) const;
  int currentPier(const Ferry& f) const { return f.phase == Ferry::Phase::Docked ? (f.dir > 0 ? f.pier_a : f.pier_b) : -1; }
  int destinationPier(const Ferry& f) const { return f.dir > 0 ? f.pier_b : f.pier_a; }
  void fastForwardOffmap(int id);  // the player is aboard while the ship is at the mainland
  void shiftOrigin(const rj::geo::Rigid3d& X) {
    for (auto& f : ships_)
      for (auto& p : f.trail) p = X.apply(p);
  }

 private:
  void pointAt(const Ferry& f, double s, rj::geo::Vec3d& p, double& heading) const;
  void startTrip(Ferry& f);
  std::vector<Pier> piers_;
  struct Route {
    std::vector<rj::geo::Geodetic> via;  // intermediate points (between the pier heads)
    bool offmap_end = false;
    std::string kind = "short";          // short | jet (high-speed) | car
  };
  std::vector<Route> routes_;
  std::vector<Ferry> ships_;
  bool placed_ = false;
};

}  // namespace rjc
