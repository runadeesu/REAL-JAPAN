#pragma once
// Residents of the slice, simulated with rjcore daily plans.
// Residents are fictional people; their homes and workplaces are real
// buildings chosen by the pipeline (pipeline/cook_slice.py -> residents.csv).
// 3D NPC rendering is NOT implemented yet; this drives the "Town Life" app.

#include <array>
#include <filesystem>
#include <vector>

#include "rj/sim/schedule.hpp"

namespace rjc {

class TownSim {
 public:
  bool load(const std::filesystem::path& csv);
  size_t size() const { return npcs_.size(); }
  // Activity histogram at a JST date/minute (plans are regenerated per day).
  std::array<int, static_cast<size_t>(rj::sim::ActivityType::kCount)> histogram(const rj::sim::CivilDate& d, int minute);

 private:
  void ensurePlans(const rj::sim::CivilDate& d);
  std::vector<rj::sim::Npc> npcs_;
  std::vector<rj::sim::DailyPlan> plans_;
  rj::sim::CivilDate plan_date_{0, 0, 0};
};

}  // namespace rjc
