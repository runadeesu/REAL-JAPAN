#pragma once
// Aviation between the fictional country's airports (data/world/country/transport.txt: per airport
// runway, taxiways, apron, stands, terminal).
//
//  * Scheduled regional jets (a fictional airline, generic aircraft) flying between the capital's
//    airport and the southern island's: boarding at the stand, pushback, taxi along the taxiway
//    layout, take-off roll and rotation, climb-out, a turn onto the route, cruise, descent, a
//    3-degree approach, flare, landing roll and taxi to a stand at the destination; after the
//    turnaround the flight goes back. The player can buy a ticket at either terminal and ride at a
//    window seat.
//  * A light aircraft (generic high-wing four-seater) parked on the apron that the player can fly:
//    six-degree-of-freedom rigid body with lift (angle of attack, stall), induced and parasitic
//    drag, propeller thrust falling with airspeed, side force; pitch / roll / yaw moments with
//    static stability and damping, elevator / aileron / rudder / flaps; tricycle landing gear with
//    springs, wheel brakes and nose-wheel steering; crashes on hard impacts and into buildings.
// The coefficients are textbook values for this class of aeroplane, not a certified model.

#include <filesystem>
#include <string>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

struct Airport {
  bool ok = false;
  std::string name, name_en;
  rj::geo::Vec3d rwy_a, rwy_b;  // thresholds (origin ENU, ground level)
  double rwy_width = 45;
  rj::geo::Vec3d twy_a, twy_b;
  std::vector<std::pair<rj::geo::Vec3d, rj::geo::Vec3d>> connectors, apron_links;  // runway->taxiway, taxiway->apron
  rj::geo::Vec3d lane_a, lane_b;  // apron taxilane
  struct Stand {
    rj::geo::Vec3d pos;
    double heading;  // nose (degrees)
  };
  std::vector<Stand> stands;
  rj::geo::Vec3d terminal;
  rj::geo::Vec3d ga_stand;  // light aircraft parking
  double ga_heading = 0;
  std::vector<std::pair<std::string, std::vector<double>>> raw;  // transport.txt lines (geodetic)
};

// A scheduled flight (regional jet).
struct Airliner {
  int id = 0;
  int stand = 0;       // stand at the airport it is at (`from` before departure, `to` after landing)
  int from = 0, to = 1;  // this leg (airport indices)
  enum class Phase { AtStand, Pushback, TaxiOut, Takeoff, Climb, Offmap, Approach, Landing, TaxiIn } phase = Phase::AtStand;
  double timer = 0;
  std::vector<rj::geo::Vec3d> path;  // current leg
  std::vector<double> cum;
  double s = 0, v = 0;
  rj::geo::Vec3d pos;  // fuselage reference point (on the centre line)
  float yaw = 0, pitch = 0, roll = 0;
  float gear = 1;      // 1 down .. 0 up
  float flaps = 0;     // 0..1
  bool lights = true;
  bool player_aboard = false;
  bool player_seated = false;  // (the flight waits at the stand until the player aboard has sat down)
};

// The player's light aircraft (6-DOF).
struct PlaneControls {
  float elevator = 0, aileron = 0, rudder = 0;  // -1..1 (elevator + = nose up)
  float throttle = 0;                            // 0..1
  int flaps = 0;                                 // notch 0..3 (0/10/20/30 degrees)
  bool brake = false;
};

class LightPlane {
 public:
  void reset(const rj::geo::Vec3d& pos, double heading_deg, const World& world);
  void update(double dt, const World& world, const PlaneControls& in, float wind_ms);
  bool crashed() const { return crashed_; }
  bool onGround() const { return on_ground_; }
  const rj::geo::Vec3d& pos() const { return pos_; }
  rj::geo::Vec3d velocity() const { return vel_; }
  double airspeed() const { return airspeed_; }  // m/s (true airspeed)
  double alpha() const { return alpha_; }
  bool stalled() const { return stall_; }
  double heading() const;    // degrees
  double pitchDeg() const;
  double rollDeg() const;
  double verticalSpeed() const { return vel_.z; }
  float prop() const { return prop_angle_; }
  float rpm() const { return rpm_; }
  const PlaneControls& controls() const { return ctl_; }
  // orientation axes in origin ENU
  rj::geo::Vec3d fwd() const { return f_; }
  rj::geo::Vec3d right() const { return r_; }
  rj::geo::Vec3d up() const { return u_; }
  Matrix modelMatrix() const;  // raylib space
  Camera3D camera(float fov, bool cockpit, float look_yaw, float look_pitch) const;
  void shiftOrigin(const rj::geo::Rigid3d& X);

 private:
  void step(double h, const World& world, float wind_ms);
  rj::geo::Vec3d pos_, vel_;
  rj::geo::Vec3d f_{0, 1, 0}, r_{1, 0, 0}, u_{0, 0, 1};
  double p_ = 0, q_ = 0, yr_ = 0;  // roll, pitch, yaw rates (body)
  PlaneControls ctl_, cmd_;
  double airspeed_ = 0, alpha_ = 0;
  bool stall_ = false, on_ground_ = true, crashed_ = false;
  bool snapped_ = false;  // wheels put on the ground once the terrain there is loaded
  float prop_angle_ = 0, rpm_ = 0;
  rj::geo::Vec3d gust_{};    // turbulence: gusts about the steady wind (a random walk; game model)
  uint32_t grng_ = 0x9e3779b9u;
  rj::geo::Vec3d cam_pos_{};
  bool cam_init_ = false;
};

class Aviation {
 public:
  bool load(const std::filesystem::path& transport, std::string& err);
  bool loaded() const { return !airports_.empty() && airports_.front().ok; }
  void place(const World& world);
  void update(double dt, const World& world);
  const Airport& airport() const { return airports_.front(); }  // the capital's (light aircraft)
  const std::vector<Airport>& airports() const { return airports_; }
  const std::vector<Airliner>& airliners() const { return jets_; }
  const Airliner* airliner(int id) const;
  const Airliner* boardable(int airport = 0) const;  // at a stand of that airport, boarding open
  // the landside entrance of an airport's terminal (where tickets are bought) and the airport whose
  // entrance is within r of p (-1: none)
  rj::geo::Vec3d landside(int airport) const;
  int airportNear(const rj::geo::Vec3d& p, double r) const;
  void setAboard(int id, bool on);
  void setSeated(int id, bool on);
  void fastForwardOffmap(int id);
  // player's light aircraft (parked when not flown)
  LightPlane& plane() { return plane_; }
  const LightPlane& plane() const { return plane_; }
  void resetPlane(const World& world);
  void shiftOrigin(const rj::geo::Rigid3d& X);  // floating-origin rebase (jets and the light aircraft)

 private:
  void refreshHeights(const World& world);
  bool heights_ok_ = false;
  double height_retry_ = 0;
  void buildTaxiOut(Airliner& a) const;
  void buildTaxiIn(Airliner& a, double stop_along) const;
  void buildClimb(Airliner& a) const;
  void buildApproach(Airliner& a) const;
  void setPath(Airliner& a, std::vector<rj::geo::Vec3d> pts) const;
  void pointAt(const Airliner& a, double s, rj::geo::Vec3d& p, double& heading, double& grade) const;
  std::vector<Airport> airports_;
  std::vector<Airliner> jets_;
  LightPlane plane_;
  bool placed_ = false;
};

}  // namespace rjc
