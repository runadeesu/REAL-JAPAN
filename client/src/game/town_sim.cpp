#include "game/town_sim.hpp"

#include <sstream>

#include "platform/paths.hpp"

namespace rjc {

bool TownSim::load(const std::filesystem::path& csv) {
  auto t = readText(csv);
  if (!t) return false;
  std::istringstream in(*t);
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string x;
    while (std::getline(ss, x, ',')) f.push_back(x);
    if (f.size() < 5) continue;
    try {
      rj::sim::ResidentSpec spec;
      spec.id = std::stoull(f[0]);
      spec.world_seed = 20260926;
      spec.occupation_id = f[1];
      spec.home = {f[2], {std::stod(f[3]), std::stod(f[4])}};
      if (f.size() >= 8 && !f[5].empty()) spec.work = {f[5], {std::stod(f[6]), std::stod(f[7])}};
      npcs_.push_back(rj::sim::generateResident(spec));
      if (f.size() >= 9 && f[8] == "commuter") ++commuters_;
    } catch (...) {
    }
  }
  return !npcs_.empty();
}

void TownSim::ensurePlans(const rj::sim::CivilDate& d) {
  if (d == plan_date_ && plans_.size() == npcs_.size()) return;
  plans_.clear();
  plans_.reserve(npcs_.size());
  for (const auto& n : npcs_) {
    rj::sim::DayContext ctx;
    ctx.date = d;
    plans_.push_back(rj::sim::planDay(n, ctx));
  }
  plan_date_ = d;
}

std::array<int, static_cast<size_t>(rj::sim::ActivityType::kCount)> TownSim::histogram(const rj::sim::CivilDate& d,
                                                                                         int minute) {
  ensurePlans(d);
  std::array<int, static_cast<size_t>(rj::sim::ActivityType::kCount)> h{};
  for (const auto& p : plans_)
    if (const auto* a = p.at(minute)) ++h[static_cast<size_t>(a->type)];
  return h;
}

}  // namespace rjc

namespace rjc {

std::vector<WalkTrip> TownSim::walkingTrips(const rj::sim::CivilDate& d, int minute) {
  ensurePlans(d);
  std::vector<WalkTrip> out;
  for (size_t i = 0; i < plans_.size(); ++i) {
    const auto& acts = plans_[i].activities;
    for (size_t k = 0; k < acts.size(); ++k) {
      const auto& a = acts[k];
      if (minute < a.start_min || minute >= a.end_min) continue;
      if (a.type != rj::sim::ActivityType::Commute || a.mode != rj::sim::TravelMode::Walk || k == 0) break;
      WalkTrip t;
      t.npc = i;
      t.start_min = a.start_min;
      t.end_min = a.end_min;
      t.from = acts[k - 1].place;
      t.to = a.place;
      if (k + 1 < acts.size()) t.next = acts[k + 1].type;
      if (t.from.valid() && t.to.valid()) out.push_back(t);
      break;
    }
  }
  return out;
}

}  // namespace rjc
