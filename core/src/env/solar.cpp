#include "rj/env/solar.hpp"

#include <cmath>

namespace rj::env {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;
constexpr double kR2D = 180.0 / kPi;
double wrap360(double x) {
  x = std::fmod(x, 360.0);
  return x < 0 ? x + 360.0 : x;
}
}  // namespace

SunPosition sunPosition(int64_t unix_utc, double lat_deg, double lon_deg) {
  const double jd = static_cast<double>(unix_utc) / 86400.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;

  const double L0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
  const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  const double Mr = M * kD2R;
  const double C = std::sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                   std::sin(2 * Mr) * (0.019993 - 0.000101 * T) + std::sin(3 * Mr) * 0.000289;
  const double true_long = L0 + C;
  const double omega = 125.04 - 1934.136 * T;
  const double lambda = true_long - 0.00569 - 0.00478 * std::sin(omega * kD2R);
  const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  const double eps = eps0 + 0.00256 * std::cos(omega * kD2R);
  const double decl = std::asin(std::sin(eps * kD2R) * std::sin(lambda * kD2R));

  const double y = std::pow(std::tan(eps * kD2R / 2.0), 2);
  const double L0r = L0 * kD2R;
  const double eot = 4.0 * kR2D *
                     (y * std::sin(2 * L0r) - 2 * e * std::sin(Mr) + 4 * e * y * std::sin(Mr) * std::cos(2 * L0r) -
                      0.5 * y * y * std::sin(4 * L0r) - 1.25 * e * e * std::sin(2 * Mr));

  const double minutes_utc = std::fmod(static_cast<double>(unix_utc), 86400.0) / 60.0;
  const double tst = std::fmod(minutes_utc + eot + 4.0 * lon_deg + 1440.0 * 2, 1440.0);
  const double ha = (tst / 4.0 < 0 ? tst / 4.0 + 180.0 : tst / 4.0 - 180.0) * kD2R;

  const double lat = lat_deg * kD2R;
  double cos_z = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(ha);
  cos_z = std::fmax(-1.0, std::fmin(1.0, cos_z));
  const double zenith = std::acos(cos_z);
  double elev = 90.0 - zenith * kR2D;

  // Approximate atmospheric refraction (NOAA).
  double refr = 0.0;
  if (elev < 85.0) {
    const double te = std::tan(elev * kD2R);
    if (elev > 5.0) refr = 58.1 / te - 0.07 / (te * te * te) + 0.000086 / std::pow(te, 5);
    else if (elev > -0.575) refr = 1735.0 + elev * (-518.2 + elev * (103.4 + elev * (-12.79 + elev * 0.711)));
    else refr = -20.772 / te;
    refr /= 3600.0;
  }
  elev += refr;

  const double az = std::atan2(std::sin(ha), std::cos(ha) * std::sin(lat) - std::tan(decl) * std::cos(lat));
  return {elev, wrap360(az * kR2D + 180.0), decl * kR2D, eot};
}

}  // namespace rj::env
