#include "rj/sim/sim_lod.hpp"

#include <algorithm>

namespace rj::sim {

void SimLodScheduler::setRegions(std::vector<SimRegion> regions) {
  regions_ = std::move(regions);
  state_.assign(regions_.size(), State{});
  for (size_t i = 0; i < regions_.size(); ++i) {
    // Deterministic stagger from the id (Knuth multiplicative hash).
    const uint32_t h = regions_[i].id * 2654435761u;
    state_[i].phase_s = (h % 1000u) / 1000.0;
  }
}

std::vector<LodTransition> SimLodScheduler::assign(const geo::LatLon& player, uint8_t player_prefecture) {
  std::vector<LodTransition> out;
  for (size_t i = 0; i < regions_.size(); ++i) {
    const auto& r = regions_[i];
    const double d = std::max(0.0, geo::approxDistanceM(player, r.center) - r.radius_m);
    SimLod lod;
    if (d <= cfg_.lod0_radius_m) lod = SimLod::LOD0;
    else if (d <= cfg_.lod1_radius_m) lod = SimLod::LOD1;
    else if (r.prefecture == player_prefecture) lod = SimLod::LOD2;
    else lod = SimLod::LOD3;
    if (lod != state_[i].lod) {
      out.push_back({r.id, state_[i].lod, lod});
      state_[i].lod = lod;
    }
  }
  return out;
}

std::vector<TickTask> SimLodScheduler::step(double game_dt_s) {
  std::vector<TickTask> out;
  std::vector<size_t> due[4];
  for (size_t i = 0; i < regions_.size(); ++i) {
    auto& s = state_[i];
    s.accum_s += game_dt_s;
    const int l = static_cast<int>(s.lod);
    const double interval = cfg_.tick_interval_s[l];
    // The first tick of a low-LOD region is offset by its phase fraction.
    if (s.accum_s + s.phase_s * interval >= interval) due[l].push_back(i);
  }
  const int limits[4] = {1 << 30, 1 << 30, cfg_.max_lod2_ticks_per_step, cfg_.max_lod3_ticks_per_step};
  for (int l = 0; l < 4; ++l) {
    auto& list = due[l];
    if (list.empty()) continue;
    // Round-robin start so throttled regions are served fairly.
    const size_t start = rr_cursor_[l] % list.size();
    const size_t n = std::min(list.size(), static_cast<size_t>(limits[l]));
    for (size_t k = 0; k < n; ++k) {
      const size_t i = list[(start + k) % list.size()];
      out.push_back({regions_[i].id, state_[i].lod, state_[i].accum_s});
      state_[i].accum_s = 0.0;
      state_[i].phase_s = 0.0;
    }
    rr_cursor_[l] = start + n;
  }
  return out;
}

SimLod SimLodScheduler::lodOf(uint32_t region_id) const {
  for (size_t i = 0; i < regions_.size(); ++i)
    if (regions_[i].id == region_id) return state_[i].lod;
  return SimLod::LOD3;
}

size_t SimLodScheduler::countAt(SimLod lod) const {
  return static_cast<size_t>(
      std::count_if(state_.begin(), state_.end(), [&](const State& s) { return s.lod == lod; }));
}

}  // namespace rj::sim
