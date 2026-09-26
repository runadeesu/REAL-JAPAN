#pragma once
// Geodetic primitives for JGD2011 (GRS80 ellipsoid).
//
// Canonical coordinates in the World Database are JGD2011 geodetic
// (latitude, longitude in degrees; height in metres). Heights from PLATEAU /
// GSI are orthometric (T.P., Tokyo Bay mean sea level); converting to
// ellipsoidal height requires a geoid model (see geoid.hpp).

#include <cmath>

namespace rj::geo {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

struct Ellipsoid {
  double a;  // semi-major axis [m]
  double f;  // flattening

  constexpr double b() const { return a * (1.0 - f); }
  constexpr double e2() const { return f * (2.0 - f); }
  constexpr double n() const { return f / (2.0 - f); }  // third flattening
};

// GRS80: a = 6378137 m, 1/f = 298.257222101 (JGD2000 / JGD2011).
inline constexpr Ellipsoid kGRS80{6378137.0, 1.0 / 298.257222101};

struct LatLon {
  double lat_deg = 0.0;
  double lon_deg = 0.0;
};

struct Geodetic {
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  double h_ellipsoidal_m = 0.0;
};

struct Ecef {
  double x = 0.0, y = 0.0, z = 0.0;
};

Ecef geodeticToEcef(const Geodetic& g, const Ellipsoid& e = kGRS80);
// Iterative (Bowring start + Newton refinement); sub-millimetre for terrestrial points.
Geodetic ecefToGeodetic(const Ecef& p, const Ellipsoid& e = kGRS80);

// Vincenty-free approximation good enough for streaming distance checks:
// local equirectangular distance in metres (error < 0.5% under ~50 km).
double approxDistanceM(const LatLon& a, const LatLon& b);

}  // namespace rj::geo
