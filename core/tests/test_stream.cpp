#include <set>
#include <vector>

#include "rj/stream/streamer.hpp"
#include "rj_test.hpp"

using namespace rj::stream;
using rj::geo::LatLon;
using rj::geo::MeshCode;

namespace {
struct FakeIO : ICellIO {
  std::vector<StreamKey> loads, unloads;
  void requestLoad(const StreamKey& k) override { loads.push_back(k); }
  void requestUnload(const StreamKey& k) override { unloads.push_back(k); }
};

// Complete all in-flight loads (simulates the async loader finishing).
void completeAll(HierarchicalStreamer& s, FakeIO& io, size_t bytes = 1u << 20) {
  for (const auto& k : io.loads) s.onLoaded(k, bytes);
  io.loads.clear();
}

std::vector<LayerConfig> twoLayers() {
  return {{"mid", 2, 12000.0, 16000.0, 0.0, 0.0, 1u << 20},
          {"city", 3, 1200.0, 1600.0, 0.0, 0.0, 1u << 20}};
}
}  // namespace

RJ_TEST(streamer_loads_coarse_before_fine) {
  FakeIO io;
  HierarchicalStreamer s(twoLayers(), 1u << 30, 1000, io);
  const Viewer v{{35.658, 139.7016}};
  s.update(v);
  // First pass: only layer-0 cells can load (no parents resident yet for layer 1).
  RJ_CHECK(!io.loads.empty());
  for (const auto& k : io.loads) RJ_CHECK_EQ(k.layer, 0);
  RJ_CHECK(s.stats().deferred_by_parent > 0);
  completeAll(s, io);
  s.update(v);
  bool any_city = false;
  for (const auto& k : io.loads) any_city |= (k.layer == 1);
  RJ_CHECK(any_city);
  completeAll(s, io);
  // The player's own 1 km cell is resident.
  RJ_CHECK(s.state({1, *MeshCode::fromLatLon(v.pos, 3)}) == CellState::Resident);
}

RJ_TEST(streamer_hysteresis_and_unload) {
  FakeIO io;
  HierarchicalStreamer s(twoLayers(), 1u << 30, 1000, io);
  Viewer v{{35.658, 139.7016}};
  for (int i = 0; i < 3; ++i) {
    s.update(v);
    completeAll(s, io);
  }
  const StreamKey home{1, *MeshCode::fromLatLon(v.pos, 3)};
  RJ_CHECK(s.state(home) == CellState::Resident);
  // Move ~2.2 km east: the old cell is between load (1.2 km) and unload (1.6 km)? No -> beyond unload.
  v.pos.lon_deg += 0.03;  // ~2.7 km
  s.update(v);
  bool unloaded_home = false;
  for (const auto& k : io.unloads) unloaded_home |= (k == home);
  RJ_CHECK(unloaded_home);
  // Small back-and-forth moves inside the hysteresis band must not thrash.
  FakeIO io2;
  HierarchicalStreamer s2(twoLayers(), 1u << 30, 1000, io2);
  Viewer w{{35.658, 139.7016}};
  for (int i = 0; i < 3; ++i) {
    s2.update(w);
    completeAll(s2, io2);
  }
  io2.unloads.clear();
  for (int i = 0; i < 10; ++i) {
    w.pos.lon_deg += (i % 2 ? -1 : 1) * 0.002;  // ±180 m
    s2.update(w);
    completeAll(s2, io2);
  }
  RJ_CHECK(io2.unloads.empty());
}

RJ_TEST(streamer_respects_budget_and_inflight) {
  FakeIO io;
  // Budget fits only a handful of cells.
  HierarchicalStreamer s(twoLayers(), 6u << 20, 3, io);
  const Viewer v{{35.658, 139.7016}};
  s.update(v);
  RJ_CHECK(io.loads.size() <= 3u);
  for (int i = 0; i < 6; ++i) {
    completeAll(s, io);
    s.update(v);
  }
  RJ_CHECK(s.stats().resident_bytes <= (6u << 20));
}

RJ_TEST(streamer_velocity_lookahead_prefetches) {
  FakeIO io;
  std::vector<LayerConfig> layers{{"city", 3, 600.0, 900.0, 60.0, 0.0, 1u << 20}};
  HierarchicalStreamer s(layers, 1u << 30, 1000, io);
  Viewer v{{35.658, 139.7016}};
  v.vel_north_mps = 80.0;  // express train / fast car: 60 s look-ahead = 4.8 km
  s.update(v);
  const MeshCode ahead = *MeshCode::fromLatLon({35.658 + 4800.0 / 110950.0, 139.7016}, 3);
  bool prefetched = false;
  for (const auto& k : io.loads) prefetched |= (k.cell == ahead);
  RJ_CHECK(prefetched);
}

RJ_TEST(interior_streaming_requires_exterior_and_proximity) {
  struct IO : IInteriorIO {
    std::set<std::string> loaded;
    void loadInterior(const std::string& b) override { loaded.insert(b); }
    void unloadInterior(const std::string& b) override { loaded.erase(b); }
  } io;
  InteriorStreamer is(40.0, 60.0, 64u << 20, io);
  const MeshCode cell = *MeshCode::parse("53393596");
  std::vector<InteriorCandidate> cands{{"bldg-A", cell, {{35.6590, 139.7010}}, 4u << 20}};
  bool exterior = false;
  auto ext = [&](const MeshCode&) { return exterior; };
  is.update({35.6590, 139.70105}, cands, ext);
  RJ_CHECK(!is.isLoaded("bldg-A"));  // exterior cell not resident yet
  exterior = true;
  is.update({35.6590, 139.70105}, cands, ext);
  RJ_CHECK(is.isLoaded("bldg-A"));
  is.update({35.6600, 139.7010}, cands, ext);  // ~110 m away -> beyond unload radius
  RJ_CHECK(!is.isLoaded("bldg-A"));
}
