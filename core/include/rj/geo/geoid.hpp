#pragma once
// Geoid models: orthometric height (T.P.) <-> ellipsoidal height.
//
// Production target: GSI "日本のジオイド2011" (GSIGEO2011) grid, bilinear
// interpolation. The grid file itself is distributed by GSI under its own
// terms and is NOT bundled; GridGeoid loads the published ASCII layout.
// ConstantGeoid exists only for tests and offline tools and must never be
// used for published geometry (isApproximate() == true).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rj/geo/ellipsoid.hpp"

namespace rj::geo {

class GeoidModel {
 public:
  virtual ~GeoidModel() = default;
  // Geoid undulation N [m] such that h_ellipsoidal = H_orthometric + N.
  virtual std::optional<double> undulation(const LatLon& p) const = 0;
  virtual bool isApproximate() const = 0;
  virtual std::string name() const = 0;
};

class ConstantGeoid final : public GeoidModel {
 public:
  explicit ConstantGeoid(double n_m) : n_(n_m) {}
  std::optional<double> undulation(const LatLon&) const override { return n_; }
  bool isApproximate() const override { return true; }
  std::string name() const override { return "constant(approximate)"; }

 private:
  double n_;
};

// Regular lat/lon grid. Values at or below `missing_threshold` are no-data.
class GridGeoid final : public GeoidModel {
 public:
  GridGeoid(double lat0, double lon0, double dlat, double dlon, int nlat, int nlon,
            std::vector<double> values, std::string name, double missing_threshold = 900.0);

  // GSIGEO2011 ASCII: header "lat0 lon0 dlat dlon nlat nlon ikind version"
  // (spacing in degrees), then nlat rows (south to north) of nlon values;
  // 999.0000 = no data.
  static std::unique_ptr<GridGeoid> loadGsigeoAscii(const std::string& path);

  std::optional<double> undulation(const LatLon& p) const override;
  bool isApproximate() const override { return false; }
  std::string name() const override { return name_; }

 private:
  double lat0_, lon0_, dlat_, dlon_;
  int nlat_, nlon_;
  std::vector<double> v_;
  std::string name_;
  double missing_;
};

}  // namespace rj::geo
