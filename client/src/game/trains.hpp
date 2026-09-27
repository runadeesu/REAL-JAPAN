#pragma once
// Trains on the fictional island's lines (data/world/island/rail.txt): the elevated loop line, the
// branch line and the Shinkansen. Headway operation (no published timetable: it is a fictional
// railway) with realistic acceleration, braking to the platform stop mark and dwell times.
// The player enters a station through the gates, boards a stopped train, rides and alights.

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
  std::vector<rj::geo::Geodetic> geo;  // rail level (h = height above sea)
  std::vector<rj::geo::Vec3d> pts;     // origin ENU
  std::vector<double> cum;
  double length = 0;
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

struct Train {
  int id = 0;
  int line = 0;
  int dir = 1;          // +1 along the polyline, -1 against
  double s = 0;         // front of the train
  double v = 0;
  int next_stop = -1;   // station index
  double dwell = 0;     // remaining stop time
  int at_station = -1;  // stopped with doors open at this station
  int cars = 10;
  double car_len = 20.0;
  double vmax = 22.0;
  double hold = 0;      // turnaround / off-map wait
  bool offmap = false;  // Shinkansen beyond the island (towards the mainland)
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
  const std::vector<Train>& trains() const { return trains_; }
  const Train* train(int id) const;
  // car pose (origin ENU, compass yaw, pitch) of car k (0 = leading car)
  void carPose(const Train& t, int k, rj::geo::Vec3d& pos, float& yaw, float& pitch) const;
  int stationNear(const rj::geo::Vec3d& p, double r) const;  // by platform centre, any height
  int trainStoppedAt(int station) const;                      // train id or -1
  std::string destination(const Train& t) const;
  int nextStation(const Train& t) const { return t.next_stop; }
  // player driving
  void setManual(int id, bool on);
  void setNotch(int id, int notch);
  bool openDoors(int id, double& stop_error);  // stopped near the next stop mark: open, dwell
  double distToStop(const Train& t) const;     // metres ahead to the next stop mark
  bool atsActive(const Train& t) const { return t.manual && ats_[static_cast<size_t>(t.id) % 64]; }
  // Platform side offset of the tracks for a station's line (metres from the line centre).
  static double platformOffset(LineKind k) { return k == LineKind::Shinkansen ? 7.4 : 6.6; }
  static double trackOffset(LineKind k) { return k == LineKind::Shinkansen ? 3.15 : 2.5; }

 private:
  void pointAt(const RailLine& L, double s, rj::geo::Vec3d& p, double& heading, double& grade) const;
  void chooseNextStop(Train& t) const;
  double stopMark(const Train& t, int station) const;
  std::vector<RailLine> lines_;
  std::vector<Station> stations_;
  std::vector<Train> trains_;
  bool placed_ = false;
  bool ats_[64] = {};
};

}  // namespace rjc
