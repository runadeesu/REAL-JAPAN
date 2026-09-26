#include "rj/stream/streamer.hpp"

#include <algorithm>
#include <cmath>

namespace rj::stream {
namespace {

double metersPerDegLat(double lat_deg) {
  const double p = lat_deg * geo::kDegToRad;
  return 111132.954 - 559.822 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
}
double metersPerDegLon(double lat_deg) {
  const double p = lat_deg * geo::kDegToRad;
  return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
}

geo::LatLon offset(const geo::LatLon& p, double east_m, double north_m) {
  return {p.lat_deg + north_m / metersPerDegLat(p.lat_deg),
          p.lon_deg + east_m / metersPerDegLon(p.lat_deg)};
}

double pointToBoxM(const geo::LatLon& p, const geo::GeoBBox& b) {
  const geo::LatLon q{std::clamp(p.lat_deg, b.min_lat, b.max_lat),
                      std::clamp(p.lon_deg, b.min_lon, b.max_lon)};
  return geo::approxDistanceM(p, q);
}

double effectiveRadius(const LayerConfig& L, double r, const Viewer& v) {
  return r * (1.0 + L.altitude_radius_gain * std::max(0.0, v.altitude_m));
}

}  // namespace

HierarchicalStreamer::HierarchicalStreamer(std::vector<LayerConfig> layers, size_t memory_budget_bytes,
                                           int max_inflight, ICellIO& io)
    : layers_(std::move(layers)), budget_(memory_budget_bytes), max_inflight_(max_inflight), io_(io) {}

std::vector<LayerConfig> HierarchicalStreamer::defaultLayers() {
  return {
      {"terrain_far", 1, 150000.0, 190000.0, 30.0, 0.0005, 24u << 20},
      {"terrain_mid", 2, 20000.0, 26000.0, 15.0, 0.002, 32u << 20},
      {"city_lod1", 3, 3000.0, 3800.0, 8.0, 0.004, 48u << 20},
      {"city_lod2", 4, 800.0, 1000.0, 5.0, 0.0, 64u << 20},
      {"detail", 6, 250.0, 320.0, 3.0, 0.0, 24u << 20},
  };
}

double HierarchicalStreamer::distanceToCell(const Viewer& v, const LayerConfig& L,
                                            const geo::MeshCode& c) const {
  const geo::GeoBBox b = c.bounds();
  const geo::LatLon ahead =
      offset(v.pos, v.vel_east_mps * L.lookahead_s, v.vel_north_mps * L.lookahead_s);
  return std::min(pointToBoxM(v.pos, b), pointToBoxM(ahead, b));
}

bool HierarchicalStreamer::parentResident(const StreamKey& k) const {
  if (k.layer == 0) return true;
  const int parent_layer = k.layer - 1;
  const StreamKey p{parent_layer, k.cell.parent(layers_[parent_layer].mesh_level)};
  auto it = cells_.find(p);
  return it != cells_.end() && it->second.state == CellState::Resident;
}

bool HierarchicalStreamer::hasResidentChildren(const StreamKey& k) const {
  const int child_layer = k.layer + 1;
  if (child_layer >= static_cast<int>(layers_.size())) return false;
  const int plevel = layers_[k.layer].mesh_level;
  for (const auto& [key, e] : cells_) {
    if (key.layer != child_layer || e.state == CellState::Unloaded) continue;
    if (key.cell.parent(plevel) == k.cell) return true;
  }
  return false;
}

size_t HierarchicalStreamer::committedBytes() const {
  size_t total = 0;
  for (const auto& [k, e] : cells_)
    if (e.state != CellState::Unloaded) total += e.bytes;
  return total;
}

bool HierarchicalStreamer::evictFor(size_t bytes_needed, const Viewer& v) {
  // Candidates: resident cells outside their load radius, finest layer first,
  // then farthest first. Cells inside their load radius are never evicted here.
  std::vector<std::pair<StreamKey, double>> cand;
  for (const auto& [k, e] : cells_) {
    if (e.state != CellState::Resident) continue;
    const auto& L = layers_[k.layer];
    const double d = distanceToCell(v, L, k.cell);
    if (d > effectiveRadius(L, L.load_radius_m, v)) cand.push_back({k, d});
  }
  std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    if (a.first.layer != b.first.layer) return a.first.layer > b.first.layer;
    return a.second > b.second;
  });
  for (const auto& [k, d] : cand) {
    if (committedBytes() + bytes_needed <= budget_) return true;
    if (hasResidentChildren(k)) continue;
    auto it = cells_.find(k);
    io_.requestUnload(k);
    ++stats_.unloads_issued;
    cells_.erase(it);
  }
  return committedBytes() + bytes_needed <= budget_;
}

void HierarchicalStreamer::update(const Viewer& v) {
  stats_.deferred_by_budget = 0;
  stats_.deferred_by_parent = 0;

  // 1) Unload cells beyond their unload radius (finest layers first so
  //    parents are never dropped under resident children).
  std::vector<StreamKey> to_unload;
  for (const auto& [k, e] : cells_) {
    if (e.state != CellState::Resident) continue;
    const auto& L = layers_[k.layer];
    if (distanceToCell(v, L, k.cell) > effectiveRadius(L, L.unload_radius_m, v)) to_unload.push_back(k);
  }
  std::sort(to_unload.begin(), to_unload.end(),
            [](const StreamKey& a, const StreamKey& b) { return a.layer > b.layer; });
  for (const auto& k : to_unload) {
    if (hasResidentChildren(k)) continue;
    io_.requestUnload(k);
    ++stats_.unloads_issued;
    cells_.erase(k);
  }

  // 2) Gather wanted cells per layer, coarse first, nearest first.
  struct Want {
    StreamKey key;
    double d;
  };
  std::vector<Want> wants;
  for (int li = 0; li < static_cast<int>(layers_.size()); ++li) {
    const auto& L = layers_[li];
    const double r = effectiveRadius(L, L.load_radius_m, v);
    const double reach_e = r + std::abs(v.vel_east_mps) * L.lookahead_s;
    const double reach_n = r + std::abs(v.vel_north_mps) * L.lookahead_s;
    const geo::LatLon lo = offset(v.pos, -reach_e, -reach_n);
    const geo::LatLon hi = offset(v.pos, reach_e, reach_n);
    for (const auto& c : geo::MeshCode::cover({lo.lat_deg, lo.lon_deg, hi.lat_deg, hi.lon_deg}, L.mesh_level)) {
      const double d = distanceToCell(v, L, c);
      if (d > r) continue;
      StreamKey k{li, c};
      auto it = cells_.find(k);
      if (it != cells_.end() && it->second.state != CellState::Unloaded) continue;
      wants.push_back({k, d});
    }
  }
  std::stable_sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
    if (a.key.layer != b.key.layer) return a.key.layer < b.key.layer;
    return a.d < b.d;
  });

  // 3) Issue loads within in-flight and memory limits.
  int inflight = 0;
  for (const auto& [k, e] : cells_)
    if (e.state == CellState::Loading) ++inflight;
  for (const auto& w : wants) {
    if (inflight >= max_inflight_) break;
    if (!parentResident(w.key)) {
      ++stats_.deferred_by_parent;
      continue;
    }
    const size_t need = layers_[w.key.layer].est_bytes_per_cell;
    if (committedBytes() + need > budget_ && !evictFor(need, v)) {
      ++stats_.deferred_by_budget;
      continue;
    }
    cells_[w.key] = Entry{CellState::Loading, need, w.d};
    io_.requestLoad(w.key);
    ++stats_.loads_issued;
    ++inflight;
  }

  stats_.resident_cells = stats_.loading_cells = stats_.resident_bytes = 0;
  for (const auto& [k, e] : cells_) {
    if (e.state == CellState::Resident) {
      ++stats_.resident_cells;
      stats_.resident_bytes += e.bytes;
    } else if (e.state == CellState::Loading) {
      ++stats_.loading_cells;
    }
  }
}

void HierarchicalStreamer::onLoaded(const StreamKey& key, size_t actual_bytes) {
  auto it = cells_.find(key);
  if (it == cells_.end()) {
    // Cancelled while in flight; release immediately.
    io_.requestUnload(key);
    return;
  }
  it->second.state = CellState::Resident;
  it->second.bytes = actual_bytes;
}

void HierarchicalStreamer::onLoadFailed(const StreamKey& key) { cells_.erase(key); }

CellState HierarchicalStreamer::state(const StreamKey& key) const {
  auto it = cells_.find(key);
  return it == cells_.end() ? CellState::Unloaded : it->second.state;
}

std::vector<StreamKey> HierarchicalStreamer::residentKeys() const {
  std::vector<StreamKey> out;
  for (const auto& [k, e] : cells_)
    if (e.state == CellState::Resident) out.push_back(k);
  return out;
}

}  // namespace rj::stream
