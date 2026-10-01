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

struct WalkTrip {
  size_t npc = 0;
  int start_min = 0, end_min = 0;
  rj::sim::PlaceRef from, to;
  rj::sim::ActivityType next = rj::sim::ActivityType::HomeLeisure;  // what they do on arrival
};

class TownSim {
 public:
  bool load(const std::filesystem::path& csv);
  size_t size() const { return npcs_.size(); }
  size_t commuters() const { return commuters_; }
  const rj::sim::Npc& npc(size_t i) const { return npcs_[i]; }
  // Residents walking (Commute on foot) at this minute, from their daily plans.
  std::vector<WalkTrip> walkingTrips(const rj::sim::CivilDate& d, int minute);
  // Activity histogram at a JST date/minute (plans are regenerated per day).
  std::array<int, static_cast<size_t>(rj::sim::ActivityType::kCount)> histogram(const rj::sim::CivilDate& d, int minute);
  // what resident i is doing at this minute (HomeLeisure when the plan has nothing)
  rj::sim::ActivityType activityOf(size_t i, const rj::sim::CivilDate& d, int minute);

 private:
  void ensurePlans(const rj::sim::CivilDate& d);
  std::vector<rj::sim::Npc> npcs_;
  size_t commuters_ = 0;
  std::vector<rj::sim::DailyPlan> plans_;
  rj::sim::CivilDate plan_date_{0, 0, 0};
};

}  // namespace rjc
