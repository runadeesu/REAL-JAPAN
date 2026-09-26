#pragma once
// Hierarchical World Streaming.
//
// Japan > Region > Prefecture > Municipality > District are logical
// (always-resident metadata, a few MB nationally). Geometry streams on the
// JIS mesh grid through *layers*, coarse to fine, e.g.
//
//   layer 0  terrain_far   level 1 (80 km)  radius 150 km
//   layer 1  terrain_mid   level 2 (10 km)  radius  20 km
//   layer 2  city_lod1     level 3 (1 km)   radius   3 km
//   layer 3  city_lod2     level 4 (500 m)  radius 800 m
//   layer 4  detail        level 6 (125 m)  radius 250 m
//
// Rules:
//  * A cell of layer k is only requested when the covering cell of layer
//    k-1 is resident (no holes: something coarser is always drawn).
//  * Unload radius > load radius (hysteresis) to avoid thrashing.
//  * Look-ahead along velocity so fast vehicles/trains/aircraft prefetch.
//  * Memory budget is enforced; eviction removes finest/farthest first and
//    never evicts a cell whose children in the next layer are resident.
//  * Buildings' interiors stream separately (InteriorStreamer) and only
//    when the exterior cell is resident and the player is near an entrance.
//
// The streamer is engine-agnostic: it issues requests through ICellIO and is
// told about completion. In UE5 ICellIO maps onto World Partition / Level
// Instance streaming of cooked cell packages.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "rj/geo/mesh_code.hpp"

namespace rj::stream {

struct LayerConfig {
  std::string name;
  int mesh_level = 3;
  double load_radius_m = 1000.0;
  double unload_radius_m = 1250.0;
  double lookahead_s = 4.0;
  double altitude_radius_gain = 0.0;  // radius *= 1 + gain * altitude_m
  size_t est_bytes_per_cell = 8u << 20;
};

struct StreamKey {
  int layer = 0;
  geo::MeshCode cell;
  bool operator==(const StreamKey& o) const { return layer == o.layer && cell == o.cell; }
};

struct StreamKeyHash {
  size_t operator()(const StreamKey& k) const noexcept {
    return std::hash<geo::MeshCode>{}(k.cell) ^ (static_cast<size_t>(k.layer) * 0x9E3779B1u);
  }
};

enum class CellState : uint8_t { Unloaded, Loading, Resident };

struct Viewer {
  geo::LatLon pos;
  double altitude_m = 0.0;  // above ground
  double vel_east_mps = 0.0;
  double vel_north_mps = 0.0;
};

class ICellIO {
 public:
  virtual ~ICellIO() = default;
  virtual void requestLoad(const StreamKey& key) = 0;
  virtual void requestUnload(const StreamKey& key) = 0;
};

struct StreamStats {
  size_t resident_cells = 0;
  size_t loading_cells = 0;
  size_t resident_bytes = 0;
  size_t loads_issued = 0;
  size_t unloads_issued = 0;
  size_t deferred_by_budget = 0;
  size_t deferred_by_parent = 0;
};

class HierarchicalStreamer {
 public:
  HierarchicalStreamer(std::vector<LayerConfig> layers, size_t memory_budget_bytes, int max_inflight,
                       ICellIO& io);

  void update(const Viewer& v);
  void onLoaded(const StreamKey& key, size_t actual_bytes);
  void onLoadFailed(const StreamKey& key);

  CellState state(const StreamKey& key) const;
  std::vector<StreamKey> residentKeys() const;
  const StreamStats& stats() const { return stats_; }
  const std::vector<LayerConfig>& layers() const { return layers_; }

  static std::vector<LayerConfig> defaultLayers();

 private:
  struct Entry {
    CellState state = CellState::Unloaded;
    size_t bytes = 0;
    double last_distance_m = 0.0;
  };
  double distanceToCell(const Viewer& v, const LayerConfig& L, const geo::MeshCode& c) const;
  bool parentResident(const StreamKey& k) const;
  bool hasResidentChildren(const StreamKey& k) const;
  size_t committedBytes() const;
  bool evictFor(size_t bytes_needed, const Viewer& v);

  std::vector<LayerConfig> layers_;
  size_t budget_;
  int max_inflight_;
  ICellIO& io_;
  std::unordered_map<StreamKey, Entry, StreamKeyHash> cells_;
  StreamStats stats_;
};

// ---------------------------------------------------------------------------
// Interior streaming. Every building in the world can participate; what gets
// loaded depends on its verification status (see rj/verify/provenance.hpp):
// verified / partial interiors load their reconstructed data; buildings with
// no interior source are either closed or get a generated interior that is
// explicitly disclosed as fictional. That decision is made by the content
// layer; the streamer only moves bytes.
struct InteriorCandidate {
  std::string building_id;
  geo::MeshCode exterior_cell;           // cell whose exterior geometry must be resident
  std::vector<geo::LatLon> entrances;
  size_t interior_bytes = 4u << 20;
};

class IInteriorIO {
 public:
  virtual ~IInteriorIO() = default;
  virtual void loadInterior(const std::string& building_id) = 0;
  virtual void unloadInterior(const std::string& building_id) = 0;
};

class InteriorStreamer {
 public:
  InteriorStreamer(double load_radius_m, double unload_radius_m, size_t budget_bytes, IInteriorIO& io)
      : load_r_(load_radius_m), unload_r_(unload_radius_m), budget_(budget_bytes), io_(io) {}

  // `exterior_resident(cell)` must say whether the exterior cell is resident.
  template <class ExteriorResidentFn>
  void update(const geo::LatLon& player, const std::vector<InteriorCandidate>& candidates,
              ExteriorResidentFn exterior_resident);

  bool isLoaded(const std::string& building_id) const { return loaded_.count(building_id) > 0; }
  size_t loadedBytes() const { return bytes_; }

 private:
  double load_r_, unload_r_;
  size_t budget_;
  IInteriorIO& io_;
  std::unordered_map<std::string, size_t> loaded_;
  size_t bytes_ = 0;
};

template <class ExteriorResidentFn>
void InteriorStreamer::update(const geo::LatLon& player,
                              const std::vector<InteriorCandidate>& candidates,
                              ExteriorResidentFn exterior_resident) {
  struct Want {
    const InteriorCandidate* c;
    double d;
  };
  std::vector<Want> wants;
  std::unordered_map<std::string, double> nearest;
  for (const auto& c : candidates) {
    double d = 1e18;
    for (const auto& e : c.entrances) d = std::min(d, geo::approxDistanceM(player, e));
    nearest[c.building_id] = d;
    if (d <= load_r_ && exterior_resident(c.exterior_cell) && !loaded_.count(c.building_id))
      wants.push_back({&c, d});
  }
  // Unload first (frees budget).
  for (auto it = loaded_.begin(); it != loaded_.end();) {
    auto n = nearest.find(it->first);
    if (n == nearest.end() || n->second > unload_r_) {
      io_.unloadInterior(it->first);
      bytes_ -= it->second;
      it = loaded_.erase(it);
    } else {
      ++it;
    }
  }
  std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
  for (const auto& w : wants) {
    if (bytes_ + w.c->interior_bytes > budget_) break;
    io_.loadInterior(w.c->building_id);
    loaded_[w.c->building_id] = w.c->interior_bytes;
    bytes_ += w.c->interior_bytes;
  }
}

}  // namespace rj::stream
