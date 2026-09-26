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
