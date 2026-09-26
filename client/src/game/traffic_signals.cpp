#include "game/traffic_signals.hpp"

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
      heads_.push_back({s.pos, s.facing_yaw, s.length, s.kind, gid, s.phase});
    }
  ngroups_ = static_cast<int>(ids.size());
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

bool TrafficSignals::flashOn() const { return std::fmod(t_, 1.0) < 0.5; }

}  // namespace rjc
