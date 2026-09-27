#pragma once
// Aviation at the fictional island's airport (data/world/island/transport.txt: runway, taxiways,
// apron, stands, terminal).
//
//  * Scheduled regional jets (a fictional airline, generic aircraft): boarding at the stand,
//    pushback, taxi along the real taxiway layout, take-off roll and rotation, climb-out and a turn
//    towards the mainland (off the map), and later a 3-degree approach, flare, landing roll and
//    taxi back to a stand. The player can buy a ticket at the terminal and ride at a window seat.
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
  int stand = 0;
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

 private:
  void step(double h, const World& world, float wind_ms);
  rj::geo::Vec3d pos_, vel_;
  rj::geo::Vec3d f_{0, 1, 0}, r_{1, 0, 0}, u_{0, 0, 1};
  double p_ = 0, q_ = 0, yr_ = 0;  // roll, pitch, yaw rates (body)
  PlaneControls ctl_, cmd_;
  double airspeed_ = 0, alpha_ = 0;
  bool stall_ = false, on_ground_ = true, crashed_ = false;
  float prop_angle_ = 0, rpm_ = 0;
  rj::geo::Vec3d cam_pos_{};
  bool cam_init_ = false;
};

class Aviation {
 public:
  bool load(const std::filesystem::path& transport, std::string& err);
  bool loaded() const { return airport_.ok; }
  void place(const World& world);
  void update(double dt, const World& world);
  const Airport& airport() const { return airport_; }
  const std::vector<Airliner>& airliners() const { return jets_; }
  const Airliner* airliner(int id) const;
  const Airliner* boardable() const;  // at a stand, boarding open
  void setAboard(int id, bool on);
  void fastForwardOffmap(int id);
  // player's light aircraft (parked when not flown)
  LightPlane& plane() { return plane_; }
  const LightPlane& plane() const { return plane_; }
  void resetPlane(const World& world);

 private:
  void buildTaxiOut(Airliner& a) const;
  void buildTaxiIn(Airliner& a, double stop_along) const;
  void buildClimb(Airliner& a) const;
  void buildApproach(Airliner& a) const;
  void setPath(Airliner& a, std::vector<rj::geo::Vec3d> pts) const;
  void pointAt(const Airliner& a, double s, rj::geo::Vec3d& p, double& heading, double& grade) const;
  Airport airport_;
  std::vector<Airliner> jets_;
  LightPlane plane_;
  bool placed_ = false;
  double ground_z_ = 0;
};

}  // namespace rjc
