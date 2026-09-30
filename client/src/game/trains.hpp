#pragma once
// Trains on the fictional country's lines (data/world/country/rail.txt): the capital's elevated
// loop line, the conventional main line and the Shinkansen lines. Headway operation (no published
// timetable: it is a fictional railway) with realistic acceleration, braking to the platform stop
// mark, speed limits on curves (from the track's curvature) and dwell times; trains reverse at the
// ends of their lines. The player enters a station through the gates, boards a stopped train,
// rides and alights.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

enum class LineKind : int { Loop = 0, Branch = 1, Shinkansen = 2 };

struct RailLine {
  LineKind kind = LineKind::Loop;
  bool closed = false;
  std::string name, name_en;           // e.g. 秋津新幹線 / Akitsu Shinkansen
  std::vector<rj::geo::Geodetic> geo;  // rail level (h = height above sea)
  std::vector<rj::geo::Vec3d> pts;     // origin ENU
  std::vector<double> cum;
  double length = 0;
  // speed limits: per point from the curve radius (v = sqrt(a * R)), and the braking envelopes a
  // train running along (+) / against (-) the polyline must stay under to meet them
  std::vector<double> vlim, env_fwd, env_bwd;
};

struct Station {
  int line = 0;
  std::string name;
  rj::geo::Geodetic geo;
  double heading = 0;  // degrees (platform axis)
  double ztop_h = 0;   // platform top, height above sea
  rj::geo::Vec3d pos;  // origin ENU at platform level
  double s = 0;        // position along the line
};

// A station's line of IC ticket gates (walk-in concourse under the platforms): the row across the
// concourse at `pos` (floor level), facing `heading` (the paid side lies ahead), with the lanes'
// offsets to the right of the heading.
struct StationGate {
  int station = -1;
  rj::geo::Geodetic geo;
  double heading = 0;  // degrees
  std::vector<float> lanes;
  rj::geo::Vec3d pos;  // origin ENU
  static constexpr float kLaneHalf = 0.5f;   // half the clear width of a lane
  static constexpr float kHalfLen = 0.8f;    // half the length of the gate cabinets
};

// A level crossing of the at-grade main line: the road across the tracks, and at each approach a
// warning post (two red lamps flashing in turn, the bell) with a barrier whose arm comes down
// across the approaching lanes a few seconds after the warning starts.
struct LevelCrossing {
  struct Set {
    rj::geo::Geodetic geo;
    rj::geo::Vec3d pos;   // barrier pivot / post foot (origin ENU)
    double facing = 0;    // compass degrees the lamps face (towards the approaching traffic)
    double arm_hd = 0;    // compass degrees the lowered arm points
    double arm_len = 3.0;
  };
  int id = 0, line = 0;
  rj::geo::Geodetic geo;
  rj::geo::Vec3d pos;     // centre on the road (origin ENU)
  double road_hd = 0;     // degrees
  double half = 3.5;      // road half width
  double s = 0;           // position along the line
  std::vector<Set> sets;
  bool warning = false;   // lamps and bell
  float since = 0;        // seconds since the warning started / stopped
  float arm = 0;          // 0 raised .. 1 lowered
  bool obstacle = false;  // something (the player, the player's car) stands on the tracks here
};

struct Train {
  int id = 0;
  int line = 0;
  int dir = 1;          // +1 along the polyline, -1 against
  double s = 0;         // front of the train
  double v = 0;
  int next_stop = -1;   // station index
  double dwell = 0;     // remaining stop time
  double dwell0 = 0;    // stop time when the train stopped (door timing)
  int at_station = -1;  // stopped with doors open at this station
  int cars = 10;
  double car_len = 20.0;
  double vmax = 22.0;
  double hold = 0;      // turnaround wait at the end of the line
  bool offmap = false;  // (unused in the country: every line ends at a station on the map)
  bool manual = false;  // driven by the player (notches), with an ATS-style safety brake
  int notch = 0;        // -8 emergency, -7..-1 brake, 0 coast, 1..5 power
};

class Trains {
 public:
  bool load(const std::filesystem::path& file, std::string& err);
  bool loaded() const { return !lines_.empty(); }
  void place(const World& world);
  void update(double dt);
  const std::vector<RailLine>& lines() const { return lines_; }
  const std::vector<Station>& stations() const { return stations_; }
  const std::vector<StationGate>& gates() const { return gates_; }
  const std::vector<LevelCrossing>& crossings() const { return crossings_; }
  // Obstacle detection at the level crossings (as the real detectors): a point on the tracks of a
  // crossing sets its stop signal, and trains running towards it brake to a stop short of it.
  void markObstacles(const std::vector<rj::geo::Vec3d>& pts);
  const std::vector<Train>& trains() const { return trains_; }
  const Train* train(int id) const;
  // car pose (origin ENU, compass yaw, pitch) of car k (0 = leading car)
  void carPose(const Train& t, int k, rj::geo::Vec3d& pos, float& yaw, float& pitch) const;
  // point on a line at distance s (wraps on the loop) and the line's compass heading there
  void poseAt(int line, double s, rj::geo::Vec3d& pos, double& heading) const {
    double g;
    pointAt(lines_[static_cast<size_t>(line)], s, pos, heading, g);
  }
  int stationNear(const rj::geo::Vec3d& p, double r) const;  // by platform centre, any height
  int trainStoppedAt(int station) const;                      // train id or -1
  std::string destination(const Train& t) const;
  // speed limit (m/s) for a train at s running in direction dir (curves ahead included)
  double speedCap(const Train& t) const;
  int nextStation(const Train& t) const { return t.next_stop; }
  // player driving
  void setManual(int id, bool on);
  void setNotch(int id, int notch);
  // how far the doors are open (0 shut .. 1 open): they open just after the train stops and
  // close in the last seconds of the stop
  static float doorOpen(const Train& t) {
    if (t.at_station < 0) return 0.0f;
    const double a = (t.dwell0 - t.dwell - 0.8) / 2.2, b = (t.dwell - 1.0) / 2.5;
    return static_cast<float>(std::clamp(std::min(a, b), 0.0, 1.0));
  }
  bool openDoors(int id, double& stop_error);  // stopped near the next stop mark: open, dwell
  double distToStop(const Train& t) const;     // metres ahead to the next stop mark
  bool atsActive(const Train& t) const { return t.manual && ats_[static_cast<size_t>(t.id) % 64]; }
  // Platform side offset of the tracks for a station's line (metres from the line centre).
  static double platformOffset(LineKind k) { return k == LineKind::Shinkansen ? 7.4 : 6.6; }
  // Signed distance of `p` from the centre line of the station's line (positive: right of the
  // station heading), measured on the curve itself (platforms on curves follow the track).
  double lateral(const Station& st, const rj::geo::Vec3d& p) const;
  static double trackOffset(LineKind k) { return k == LineKind::Shinkansen ? 3.15 : 2.5; }

 private:
  void pointAt(const RailLine& L, double s, rj::geo::Vec3d& p, double& heading, double& grade) const;
  void chooseNextStop(Train& t) const;
  double stopMark(const Train& t, int station) const;
  std::vector<RailLine> lines_;
  std::vector<Station> stations_;
  std::vector<StationGate> gates_;
  std::vector<LevelCrossing> crossings_;
  void updateCrossings(double dt);
  std::vector<Train> trains_;
  bool placed_ = false;
  bool ats_[64] = {};
};

}  // namespace rjc
