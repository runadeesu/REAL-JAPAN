#include "game/traffic_signals.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "world/world.hpp"

namespace rjc {

void TrafficSignals::rebuild(const World& world) {
  heads_.clear();
  std::map<std::string, int> ids;  // (cell, group) -> global id
  for (const auto& [code, cell] : world.cells())
    for (const auto& s : cell->signals) {
      const std::string key = code + ":" + std::to_string(s.group);
      auto it = ids.find(key);
      const int gid = it == ids.end() ? (ids[key] = static_cast<int>(ids.size())) : it->second;
      heads_.push_back({s.pos, s.facing_yaw, s.length, s.kind, gid, s.phase, false});
    }
  ngroups_ = static_cast<int>(ids.size());
  heads_.insert(heads_.end(), est_.begin(), est_.end());
}

void TrafficSignals::addEstimatedHead(const Head& h) {
  Head e = h;
  e.estimated = true;
  est_.push_back(e);
  heads_.push_back(e);
}

void TrafficSignals::clearEstimated() {
  est_.clear();
  n_est_groups_ = 0;
  heads_.erase(std::remove_if(heads_.begin(), heads_.end(), [](const Head& h) { return h.estimated; }), heads_.end());
}

double TrafficSignals::local(int group) const {
  const double offset = std::fmod(group * 37.0, kCycle);  // intersections are not synchronised
  return std::fmod(t_ + offset, kCycle);
}

// Phase 0: green 0-44, yellow 44-47, all red 47-49. Phase 1: green 49-95, yellow 95-98, all red 98-100.
VehLamp TrafficSignals::vehicle(int group, int phase) const {
  const double t = local(group);
  const double g0 = phase == 0 ? 0.0 : 49.0, g1 = phase == 0 ? 44.0 : 95.0;
  if (t >= g0 && t < g1) return VehLamp::Green;
  if (t >= g1 && t < g1 + 3.0) return VehLamp::Yellow;
  return VehLamp::Red;
}

// Pedestrians cross parallel to the vehicle phase: walk, then flashing green for 8 s, then stop.
PedLamp TrafficSignals::pedestrian(int group, int phase) const {
  const double t = local(group);
  const double w0 = phase == 0 ? 0.0 : 49.0, w1 = phase == 0 ? 36.0 : 87.0;
  if (t >= w0 && t < w1) return PedLamp::Walk;
  if (t >= w1 && t < w1 + 8.0) return PedLamp::Flash;
  return PedLamp::Stop;
}

int TrafficSignals::phaseForAxis(int group, double h) const {
  // Angle between axes (mod 180 deg) to the nearest head of the group; perpendicular -> the other phase.
  double best = 10.0;
  int phase = 0;
  for (const auto& hd : heads_) {
    if (hd.group != group) continue;
    const double d = std::fabs(std::remainder(static_cast<double>(hd.facing) - h, 3.14159265358979323846));
    if (d < best) {
      best = d;
      phase = hd.phase;
    }
  }
  return best < 3.14159265358979323846 / 4 ? phase : 1 - phase;
}

bool TrafficSignals::flashOn() const { return std::fmod(t_, 1.0) < 0.5; }

}  // namespace rjc
