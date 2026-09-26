#pragma once
// Simulation LOD: the whole country keeps living, at a cost that scales with
// distance from the player.
//
//   LOD0  around the player      full 3D + full AI, every frame
//   LOD1  nearby area            simplified AI (no animation/physics), ~2 Hz
//   LOD2  same prefecture        low-frequency aggregate agents, ~1/game-minute
//   LOD3  other prefectures      statistical / event-based, ~1/game-hour
//
// Key property: NPC state at LOD2/LOD3 is *derivable* from their daily plan
// (see schedule.hpp: activityAt), so promotion LOD3 -> LOD0 materialises
// agents where their schedules say they are, without having simulated them
// step by step. Only divergent state (money, relationships, events) is
// integrated at low frequency.

#include <cstdint>
#include <functional>
#include <vector>

#include "rj/geo/ellipsoid.hpp"

namespace rj::sim {

enum class SimLod : uint8_t { LOD0 = 0, LOD1 = 1, LOD2 = 2, LOD3 = 3 };

struct SimLodConfig {
  double lod0_radius_m = 300.0;
  double lod1_radius_m = 2000.0;
  // Game-seconds between ticks per LOD. 0 = every scheduler step.
  double tick_interval_s[4] = {0.0, 0.5, 60.0, 3600.0};
  // Max number of LOD2 / LOD3 region ticks processed per scheduler step
  // (the remainder carries over; accumulated dt is never lost).
  int max_lod2_ticks_per_step = 64;
  int max_lod3_ticks_per_step = 8;
};

struct SimRegion {
  uint32_t id = 0;
  uint8_t prefecture = 0;       // JIS X 0401
  uint32_t municipality = 0;    // JIS X 0402
  geo::LatLon center;
  double radius_m = 500.0;      // extent used for LOD0/LOD1 distance tests
};

struct TickTask {
  uint32_t region_id;
  SimLod lod;
  double dt_s;  // accumulated game time since this region's previous tick
};

struct LodTransition {
  uint32_t region_id;
  SimLod from, to;
};

class SimLodScheduler {
 public:
  explicit SimLodScheduler(SimLodConfig cfg = {}) : cfg_(cfg) {}

  void setRegions(std::vector<SimRegion> regions);
  // Re-evaluate LODs for a player position; returns transitions (for
  // materialising / dematerialising agents).
  std::vector<LodTransition> assign(const geo::LatLon& player, uint8_t player_prefecture);
  // Advance game time; returns the regions that tick this step.
  std::vector<TickTask> step(double game_dt_s);

  SimLod lodOf(uint32_t region_id) const;
  size_t countAt(SimLod lod) const;
  const SimLodConfig& config() const { return cfg_; }

 private:
  struct State {
    SimLod lod = SimLod::LOD3;
    double accum_s = 0.0;
    double phase_s = 0.0;  // stagger so low-LOD ticks spread over time
  };
  SimLodConfig cfg_;
  std::vector<SimRegion> regions_;
  std::vector<State> state_;
  size_t rr_cursor_[4] = {0, 0, 0, 0};
};

}  // namespace rj::sim
