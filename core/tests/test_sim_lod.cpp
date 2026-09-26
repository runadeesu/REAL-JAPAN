#include "rj/sim/sim_lod.hpp"
#include "rj_test.hpp"

using namespace rj::sim;

namespace {
std::vector<SimRegion> regions() {
  return {
      {1, 13, 13113, {35.6590, 139.7005}, 400.0},  // Shibuya station area
      {2, 13, 13113, {35.6700, 139.7020}, 400.0},  // ~1.2 km north (Harajuku side)
      {3, 13, 13104, {35.6900, 139.7000}, 500.0},  // Shinjuku, same prefecture
      {4, 27, 27127, {34.7025, 135.4959}, 800.0},  // Osaka Umeda, other prefecture
      {5, 1, 1101, {43.0687, 141.3508}, 800.0},    // Sapporo, other prefecture
  };
}
}  // namespace

RJ_TEST(sim_lod_assignment_by_distance_and_prefecture) {
  SimLodScheduler s;
  s.setRegions(regions());
  const auto tr = s.assign({35.6590, 139.7005}, 13);
  RJ_CHECK(s.lodOf(1) == SimLod::LOD0);
  RJ_CHECK(s.lodOf(2) == SimLod::LOD1);
  RJ_CHECK(s.lodOf(3) == SimLod::LOD2);
  RJ_CHECK(s.lodOf(4) == SimLod::LOD3);
  RJ_CHECK(s.lodOf(5) == SimLod::LOD3);
  RJ_CHECK_EQ(tr.size(), 3u);  // regions 1..3 changed from the default LOD3

  // Player flies to Sapporo: Tokyo keeps running at LOD3 (never frozen).
  const auto tr2 = s.assign({43.0687, 141.3508}, 1);
  RJ_CHECK(s.lodOf(5) == SimLod::LOD0);
  RJ_CHECK(s.lodOf(1) == SimLod::LOD3);
  RJ_CHECK(!tr2.empty());
}

RJ_TEST(sim_lod_tick_rates_and_no_lost_time) {
  SimLodScheduler s;
  s.setRegions(regions());
  s.assign({35.6590, 139.7005}, 13);
  double lod3_time = 0.0, lod0_time = 0.0;
  int lod0_ticks = 0, lod2_ticks = 0;
  // Two game hours in 1-second steps.
  for (int i = 0; i < 7200; ++i) {
    for (const auto& t : s.step(1.0)) {
      if (t.region_id == 1) {
        ++lod0_ticks;
        lod0_time += t.dt_s;
      }
      if (t.region_id == 3) ++lod2_ticks;
      if (t.region_id == 4) lod3_time += t.dt_s;
    }
  }
  RJ_CHECK_EQ(lod0_ticks, 7200);           // every step
  RJ_CHECK_NEAR(lod0_time, 7200.0, 1e-6);
  RJ_CHECK(lod2_ticks >= 119 && lod2_ticks <= 121);  // once per game minute
  RJ_CHECK(lod3_time >= 3600.0);           // hourly, accumulated dt carried in the tick
}

RJ_TEST(sim_lod_throttles_low_lod_but_keeps_accumulating) {
  SimLodConfig cfg;
  cfg.max_lod3_ticks_per_step = 1;
  SimLodScheduler s(cfg);
  std::vector<SimRegion> many;
  for (uint32_t i = 0; i < 10; ++i) many.push_back({100 + i, 27, 27127, {34.70, 135.49 + 0.01 * i}, 100.0});
  s.setRegions(many);
  s.assign({35.6590, 139.7005}, 13);  // all LOD3
  auto t = s.step(4000.0);
  RJ_CHECK_EQ(t.size(), 1u);
  double total = t[0].dt_s;
  for (int i = 0; i < 9; ++i) {
    auto more = s.step(0.0);
    RJ_CHECK_EQ(more.size(), 1u);
    total += more[0].dt_s;
  }
  RJ_CHECK_NEAR(total, 40000.0, 1e-6);  // nobody lost simulated time
}
