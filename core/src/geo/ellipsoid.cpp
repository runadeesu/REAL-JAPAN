#include "rj/geo/ellipsoid.hpp"

namespace rj::geo {

Ecef geodeticToEcef(const Geodetic& g, const Ellipsoid& e) {
  const double lat = g.lat_deg * kDegToRad;
  const double lon = g.lon_deg * kDegToRad;
  const double s = std::sin(lat);
  const double N = e.a / std::sqrt(1.0 - e.e2() * s * s);
  const double h = g.h_ellipsoidal_m;
  return {(N + h) * std::cos(lat) * std::cos(lon), (N + h) * std::cos(lat) * std::sin(lon),
          (N * (1.0 - e.e2()) + h) * s};
}

Geodetic ecefToGeodetic(const Ecef& p, const Ellipsoid& e) {
  const double e2 = e.e2();
  const double b = e.b();
  const double ep2 = (e.a * e.a - b * b) / (b * b);
  const double r = std::sqrt(p.x * p.x + p.y * p.y);
  const double lon = std::atan2(p.y, p.x);
  // Bowring's initial estimate.
  const double theta = std::atan2(p.z * e.a, r * b);
  const double st = std::sin(theta), ct = std::cos(theta);
  double lat = std::atan2(p.z + ep2 * b * st * st * st, r - e2 * e.a * ct * ct * ct);
  double h = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double s = std::sin(lat);
    const double N = e.a / std::sqrt(1.0 - e2 * s * s);
    h = r / std::cos(lat) - N;
    lat = std::atan2(p.z, r * (1.0 - e2 * N / (N + h)));
  }
  return {lat * kRadToDeg, lon * kRadToDeg, h};
}

double approxDistanceM(const LatLon& a, const LatLon& b) {
  constexpr double kMetersPerDegLat = 110940.0;  // ~ at 35 deg N; adequate for LOD decisions
  const double mean_lat = 0.5 * (a.lat_deg + b.lat_deg) * kDegToRad;
  const double dy = (b.lat_deg - a.lat_deg) * kMetersPerDegLat;
  const double dx = (b.lon_deg - a.lon_deg) * kMetersPerDegLat * std::cos(mean_lat);
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace rj::geo
