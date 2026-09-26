#pragma once
// JIS X 0410 standard regional mesh (標準地域メッシュ / 地域メッシュコード).
//
// This is the national grid used by GSI, the Statistics Bureau and PLATEAU
// (PLATEAU CityGML files are partitioned by 3rd-level mesh, e.g.
// 53393586_bldg_6697_op.gml). REAL JAPAN uses it as the physical streaming
// grid so that source data partitions and runtime cells line up 1:1.
//
//   level 1: 40'   x 1°      (~80 km)   code "5339"
//   level 2: 5'    x 7'30"   (~10 km)   code "533935"
//   level 3: 30"   x 45"     (~1 km)    code "53393586"
//   level 4: 15"   x 22.5"   (~500 m)   1/2 mesh, +1 digit (1..4)
//   level 5: 7.5"  x 11.25"  (~250 m)   1/4 mesh, +1 digit
//   level 6: 3.75" x 5.625"  (~125 m)   1/8 mesh, +1 digit
//
// Internally a cell is stored as the south-west corner in level-6 units:
// one level-1 cell is exactly 640 x 640 level-6 cells.

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rj/geo/ellipsoid.hpp"

namespace rj::geo {

struct GeoBBox {
  double min_lat = 0.0, min_lon = 0.0, max_lat = 0.0, max_lon = 0.0;
  bool contains(const LatLon& p) const {
    return p.lat_deg >= min_lat && p.lat_deg < max_lat && p.lon_deg >= min_lon &&
           p.lon_deg < max_lon;
  }
  bool intersects(const GeoBBox& o) const {
    return min_lat < o.max_lat && o.min_lat < max_lat && min_lon < o.max_lon &&
           o.min_lon < max_lon;
  }
};

class MeshCode {
 public:
  static constexpr int kMinLevel = 1;
  static constexpr int kMaxLevel = 6;

  MeshCode() = default;  // level-1 cell "0000" (placeholder value)

  static std::optional<MeshCode> fromLatLon(const LatLon& p, int level);
  static std::optional<MeshCode> parse(std::string_view code);
  // Every cell of `level` intersecting the box (row-major, south to north).
  static std::vector<MeshCode> cover(const GeoBBox& box, int level);

  int level() const { return level_; }
  std::string str() const;
  uint64_t value() const;
  GeoBBox bounds() const;
  LatLon center() const;

  MeshCode parent(int level) const;  // level <= this->level()
  std::vector<MeshCode> children() const;  // empty at kMaxLevel
  // Neighbour at (d_north, d_east) cells of the same level.
  MeshCode neighbor(int d_north, int d_east) const;

  auto operator<=>(const MeshCode&) const = default;

  // Size of one cell edge at a level, in level-6 units.
  static int32_t stepL6(int level);

 private:
  MeshCode(int32_t i6, int32_t j6, int level) : i6_(i6), j6_(j6), level_(level) {}
  int32_t i6_ = 0;  // floor(lat * 960)
  int32_t j6_ = 0;  // floor((lon - 100) * 640)
  int level_ = 1;

  friend struct std::hash<MeshCode>;
};

}  // namespace rj::geo

template <>
struct std::hash<rj::geo::MeshCode> {
  size_t operator()(const rj::geo::MeshCode& m) const noexcept {
    uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(m.i6_)) << 32) ^
                 static_cast<uint32_t>(m.j6_);
    h ^= static_cast<uint64_t>(m.level_) * 0x9E3779B97F4A7C15ULL;
    return std::hash<uint64_t>{}(h);
  }
};
