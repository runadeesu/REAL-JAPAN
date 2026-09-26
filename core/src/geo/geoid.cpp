#include "rj/geo/geoid.hpp"

#include <cmath>
#include <fstream>

namespace rj::geo {

GridGeoid::GridGeoid(double lat0, double lon0, double dlat, double dlon, int nlat, int nlon,
                     std::vector<double> values, std::string name, double missing_threshold)
    : lat0_(lat0),
      lon0_(lon0),
      dlat_(dlat),
      dlon_(dlon),
      nlat_(nlat),
      nlon_(nlon),
      v_(std::move(values)),
      name_(std::move(name)),
      missing_(missing_threshold) {}

std::unique_ptr<GridGeoid> GridGeoid::loadGsigeoAscii(const std::string& path) {
  std::ifstream in(path);
  if (!in) return nullptr;
  double lat0, lon0, dlat, dlon;
  int nlat, nlon, ikind;
  std::string version;
  if (!(in >> lat0 >> lon0 >> dlat >> dlon >> nlat >> nlon >> ikind >> version)) return nullptr;
  // Rows may wrap across several text lines; whitespace-separated reading handles that.
  // TODO: verify against the real GSIGEO2011 file once obtained (not bundled; GSI terms apply).
  std::vector<double> v(static_cast<size_t>(nlat) * static_cast<size_t>(nlon));
  for (auto& x : v)
    if (!(in >> x)) return nullptr;
  return std::make_unique<GridGeoid>(lat0, lon0, dlat, dlon, nlat, nlon, std::move(v),
                                     "GSIGEO:" + version);
}

std::optional<double> GridGeoid::undulation(const LatLon& p) const {
  const double fi = (p.lat_deg - lat0_) / dlat_;
  const double fj = (p.lon_deg - lon0_) / dlon_;
  const int i = static_cast<int>(std::floor(fi));
  const int j = static_cast<int>(std::floor(fj));
  if (i < 0 || j < 0 || i + 1 >= nlat_ || j + 1 >= nlon_) return std::nullopt;
  auto at = [&](int a, int b) { return v_[static_cast<size_t>(a) * nlon_ + b]; };
  const double q00 = at(i, j), q01 = at(i, j + 1), q10 = at(i + 1, j), q11 = at(i + 1, j + 1);
  if (q00 >= missing_ || q01 >= missing_ || q10 >= missing_ || q11 >= missing_) return std::nullopt;
  const double u = fi - i, w = fj - j;
  return (1 - u) * ((1 - w) * q00 + w * q01) + u * ((1 - w) * q10 + w * q11);
}

}  // namespace rj::geo
