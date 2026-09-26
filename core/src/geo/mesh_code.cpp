#include "rj/geo/mesh_code.hpp"

#include <cmath>

namespace rj::geo {
namespace {

constexpr double kLatUnitsPerDeg = 960.0;  // level-6 rows per degree of latitude
constexpr double kLonUnitsPerDeg = 640.0;  // level-6 columns per degree of longitude
// Guards against 35.65 * 960 evaluating to 34223.999999... at exact cell edges.
constexpr double kEdgeEps = 1e-9;

int32_t floorUnits(double v) { return static_cast<int32_t>(std::floor(v + kEdgeEps)); }

}  // namespace

int32_t MeshCode::stepL6(int level) {
  switch (level) {
    case 1: return 640;
    case 2: return 80;
    case 3: return 8;
    case 4: return 4;
    case 5: return 2;
    default: return 1;
  }
}

std::optional<MeshCode> MeshCode::fromLatLon(const LatLon& p, int level) {
  if (level < kMinLevel || level > kMaxLevel) return std::nullopt;
  if (p.lat_deg < 0.0 || p.lat_deg * 1.5 >= 100.0) return std::nullopt;
  if (p.lon_deg < 100.0 || p.lon_deg >= 200.0) return std::nullopt;
  const int32_t step = stepL6(level);
  int32_t i = floorUnits(p.lat_deg * kLatUnitsPerDeg);
  int32_t j = floorUnits((p.lon_deg - 100.0) * kLonUnitsPerDeg);
  i -= i % step;
  j -= j % step;
  return MeshCode(i, j, level);
}

std::optional<MeshCode> MeshCode::parse(std::string_view code) {
  const size_t len = code.size();
  int level;
  switch (len) {
    case 4: level = 1; break;
    case 6: level = 2; break;
    case 8: level = 3; break;
    case 9: level = 4; break;
    case 10: level = 5; break;
    case 11: level = 6; break;
    default: return std::nullopt;
  }
  for (char c : code)
    if (c < '0' || c > '9') return std::nullopt;
  auto d = [&](size_t k) { return static_cast<int32_t>(code[k] - '0'); };

  int32_t i = (d(0) * 10 + d(1)) * 640;
  int32_t j = (d(2) * 10 + d(3)) * 640;
  if (level >= 2) {
    if (d(4) > 7 || d(5) > 7) return std::nullopt;
    i += d(4) * 80;
    j += d(5) * 80;
  }
  if (level >= 3) {
    i += d(6) * 8;
    j += d(7) * 8;
  }
  int32_t half = 4;
  for (size_t k = 8; k < len; ++k) {
    const int32_t q = d(k);
    if (q < 1 || q > 4) return std::nullopt;
    // 1 = SW, 2 = SE, 3 = NW, 4 = NE
    if (q >= 3) i += half;
    if (q == 2 || q == 4) j += half;
    half /= 2;
  }
  return MeshCode(i, j, level);
}

std::string MeshCode::str() const {
  const int32_t p = i6_ / 640, u = j6_ / 640;
  const int32_t i1 = i6_ % 640, j1 = j6_ % 640;
  std::string s;
  auto push2 = [&](int32_t v) {
    s.push_back(static_cast<char>('0' + v / 10));
    s.push_back(static_cast<char>('0' + v % 10));
  };
  push2(p);
  push2(u);
  if (level_ >= 2) {
    s.push_back(static_cast<char>('0' + i1 / 80));
    s.push_back(static_cast<char>('0' + j1 / 80));
  }
  if (level_ >= 3) {
    s.push_back(static_cast<char>('0' + (i1 % 80) / 8));
    s.push_back(static_cast<char>('0' + (j1 % 80) / 8));
  }
  const int32_t i3 = i1 % 8, j3 = j1 % 8;
  int32_t half = 4;
  for (int l = 4; l <= level_; ++l) {
    const bool north = (i3 & half) != 0;
    const bool east = (j3 & half) != 0;
    s.push_back(static_cast<char>('1' + (north ? 2 : 0) + (east ? 1 : 0)));
    half /= 2;
  }
  return s;
}

uint64_t MeshCode::value() const { return std::stoull(str()); }

GeoBBox MeshCode::bounds() const {
  const int32_t step = stepL6(level_);
  return {i6_ / kLatUnitsPerDeg, 100.0 + j6_ / kLonUnitsPerDeg, (i6_ + step) / kLatUnitsPerDeg,
          100.0 + (j6_ + step) / kLonUnitsPerDeg};
}

LatLon MeshCode::center() const {
  const GeoBBox b = bounds();
  return {0.5 * (b.min_lat + b.max_lat), 0.5 * (b.min_lon + b.max_lon)};
}

MeshCode MeshCode::parent(int level) const {
  if (level >= level_) return *this;
  if (level < kMinLevel) level = kMinLevel;
  const int32_t step = stepL6(level);
  return MeshCode(i6_ - i6_ % step, j6_ - j6_ % step, level);
}

std::vector<MeshCode> MeshCode::children() const {
  std::vector<MeshCode> out;
  if (level_ >= kMaxLevel) return out;
  const int child = level_ + 1;
  const int32_t cstep = stepL6(child);
  const int32_t n = stepL6(level_) / cstep;
  out.reserve(static_cast<size_t>(n * n));
  for (int32_t a = 0; a < n; ++a)
    for (int32_t b = 0; b < n; ++b) out.push_back(MeshCode(i6_ + a * cstep, j6_ + b * cstep, child));
  return out;
}

MeshCode MeshCode::neighbor(int d_north, int d_east) const {
  const int32_t step = stepL6(level_);
  return MeshCode(i6_ + d_north * step, j6_ + d_east * step, level_);
}

std::vector<MeshCode> MeshCode::cover(const GeoBBox& box, int level) {
  std::vector<MeshCode> out;
  auto sw = fromLatLon({box.min_lat, box.min_lon}, level);
  if (!sw) return out;
  const int32_t step = stepL6(level);
  const int32_t i_end = static_cast<int32_t>(std::ceil(box.max_lat * kLatUnitsPerDeg - kEdgeEps));
  const int32_t j_end =
      static_cast<int32_t>(std::ceil((box.max_lon - 100.0) * kLonUnitsPerDeg - kEdgeEps));
  for (int32_t i = sw->i6_; i < i_end; i += step)
    for (int32_t j = sw->j6_; j < j_end; j += step) out.push_back(MeshCode(i, j, level));
  return out;
}

}  // namespace rj::geo
