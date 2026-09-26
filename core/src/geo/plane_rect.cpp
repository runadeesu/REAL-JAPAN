#include "rj/geo/plane_rect.hpp"

#include <array>

namespace rj::geo {
namespace {

constexpr double kM0 = 0.9999;

// Zone origins per 平成14年国土交通省告示第9号 (validated against EPSG in tests).
constexpr std::array<PlaneRectZone, 19> kZones{{
    {1, 33.0, 129.5, 6669},
    {2, 33.0, 131.0, 6670},
    {3, 36.0, 132.0 + 10.0 / 60.0, 6671},
    {4, 33.0, 133.5, 6672},
    {5, 36.0, 134.0 + 20.0 / 60.0, 6673},
    {6, 36.0, 136.0, 6674},
    {7, 36.0, 137.0 + 10.0 / 60.0, 6675},
    {8, 36.0, 138.5, 6676},
    {9, 36.0, 139.0 + 50.0 / 60.0, 6677},
    {10, 40.0, 140.0 + 50.0 / 60.0, 6678},
    {11, 44.0, 140.25, 6679},
    {12, 44.0, 142.25, 6680},
    {13, 44.0, 144.25, 6681},
    {14, 26.0, 142.0, 6682},
    {15, 26.0, 127.5, 6683},
    {16, 26.0, 124.0, 6684},
    {17, 26.0, 131.0, 6685},
    {18, 20.0, 136.0, 6686},
    {19, 26.0, 154.0, 6687},
}};

struct Series {
  double n;
  double A_bar;               // m0 * a / (1+n) * A0
  std::array<double, 6> A;    // meridian arc coefficients A0..A5
  std::array<double, 6> alpha;  // 1-based, [0] unused
  std::array<double, 6> beta;   // 1-based, [0] unused
  std::array<double, 7> delta;  // 1-based, [0] unused
};

Series makeSeries(const Ellipsoid& e) {
  const double n = e.n();
  const double n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
  Series s{};
  s.n = n;
  s.A[0] = 1.0 + n2 / 4.0 + n4 / 64.0;
  s.A[1] = -1.5 * (n - n3 / 8.0 - n5 / 64.0);
  s.A[2] = 15.0 / 16.0 * (n2 - n4 / 4.0);
  s.A[3] = -35.0 / 48.0 * (n3 - 5.0 * n5 / 16.0);
  s.A[4] = 315.0 / 512.0 * n4;
  s.A[5] = -693.0 / 1280.0 * n5;
  s.A_bar = kM0 * e.a / (1.0 + n) * s.A[0];

  s.alpha[1] = n / 2.0 - 2.0 * n2 / 3.0 + 5.0 * n3 / 16.0 + 41.0 * n4 / 180.0 - 127.0 * n5 / 288.0;
  s.alpha[2] = 13.0 * n2 / 48.0 - 3.0 * n3 / 5.0 + 557.0 * n4 / 1440.0 + 281.0 * n5 / 630.0;
  s.alpha[3] = 61.0 * n3 / 240.0 - 103.0 * n4 / 140.0 + 15061.0 * n5 / 26880.0;
  s.alpha[4] = 49561.0 * n4 / 161280.0 - 179.0 * n5 / 168.0;
  s.alpha[5] = 34729.0 * n5 / 80640.0;

  s.beta[1] = n / 2.0 - 2.0 * n2 / 3.0 + 37.0 * n3 / 96.0 - n4 / 360.0 - 81.0 * n5 / 512.0;
  s.beta[2] = n2 / 48.0 + n3 / 15.0 - 437.0 * n4 / 1440.0 + 46.0 * n5 / 105.0;
  s.beta[3] = 17.0 * n3 / 480.0 - 37.0 * n4 / 840.0 - 209.0 * n5 / 4480.0;
  s.beta[4] = 4397.0 * n4 / 161280.0 - 11.0 * n5 / 504.0;
  s.beta[5] = 4583.0 * n5 / 161280.0;

  s.delta[1] = 2.0 * n - 2.0 * n2 / 3.0 - 2.0 * n3 + 116.0 * n4 / 45.0 + 26.0 * n5 / 45.0 -
               2854.0 * n6 / 675.0;
  s.delta[2] = 7.0 * n2 / 3.0 - 8.0 * n3 / 5.0 - 227.0 * n4 / 45.0 + 2704.0 * n5 / 315.0 +
               2323.0 * n6 / 945.0;
  s.delta[3] = 56.0 * n3 / 15.0 - 136.0 * n4 / 35.0 - 1262.0 * n5 / 105.0 + 73814.0 * n6 / 2835.0;
  s.delta[4] = 4279.0 * n4 / 630.0 - 332.0 * n5 / 35.0 - 399572.0 * n6 / 14175.0;
  s.delta[5] = 4174.0 * n5 / 315.0 - 144838.0 * n6 / 6237.0;
  s.delta[6] = 601676.0 * n6 / 22275.0;
  return s;
}

const Series& grs80Series() {
  static const Series s = makeSeries(kGRS80);
  return s;
}

// Meridian arc length from the equator to the zone origin latitude, scaled by m0.
double originArc(const Series& s, double phi0) {
  double sum = s.A[0] * phi0;
  for (int j = 1; j <= 5; ++j) sum += s.A[j] * std::sin(2.0 * j * phi0);
  return kM0 * kGRS80.a / (1.0 + s.n) * sum;
}

}  // namespace

std::optional<PlaneRectZone> planeRectZone(int number) {
  if (number < 1 || number > 19) return std::nullopt;
  return kZones[static_cast<size_t>(number - 1)];
}

PlaneXY geodeticToPlane(const LatLon& g, const PlaneRectZone& zone) {
  const Series& s = grs80Series();
  const double phi = g.lat_deg * kDegToRad;
  const double dlam = (g.lon_deg - zone.origin_lon) * kDegToRad;
  const double phi0 = zone.origin_lat * kDegToRad;

  const double k = 2.0 * std::sqrt(s.n) / (1.0 + s.n);
  const double t = std::sinh(std::atanh(std::sin(phi)) - k * std::atanh(k * std::sin(phi)));
  const double t_bar = std::sqrt(1.0 + t * t);
  const double xi_p = std::atan2(t, std::cos(dlam));
  const double eta_p = std::atanh(std::sin(dlam) / t_bar);

  double x = xi_p, y = eta_p;
  for (int j = 1; j <= 5; ++j) {
    x += s.alpha[j] * std::sin(2.0 * j * xi_p) * std::cosh(2.0 * j * eta_p);
    y += s.alpha[j] * std::cos(2.0 * j * xi_p) * std::sinh(2.0 * j * eta_p);
  }
  return {s.A_bar * x - originArc(s, phi0), s.A_bar * y};
}

LatLon planeToGeodetic(const PlaneXY& p, const PlaneRectZone& zone) {
  const Series& s = grs80Series();
  const double phi0 = zone.origin_lat * kDegToRad;
  const double xi = (p.x_north_m + originArc(s, phi0)) / s.A_bar;
  const double eta = p.y_east_m / s.A_bar;

  double xi_p = xi, eta_p = eta;
  for (int j = 1; j <= 5; ++j) {
    xi_p -= s.beta[j] * std::sin(2.0 * j * xi) * std::cosh(2.0 * j * eta);
    eta_p -= s.beta[j] * std::cos(2.0 * j * xi) * std::sinh(2.0 * j * eta);
  }
  const double chi = std::asin(std::sin(xi_p) / std::cosh(eta_p));
  double phi = chi;
  for (int j = 1; j <= 6; ++j) phi += s.delta[j] * std::sin(2.0 * j * chi);
  const double lam = zone.origin_lon * kDegToRad + std::atan2(std::sinh(eta_p), std::cos(xi_p));
  return {phi * kRadToDeg, lam * kRadToDeg};
}

}  // namespace rj::geo
