#pragma once
// Astronomical sun position (NOAA solar calculator equations, after Meeus).
// Drives the day/night cycle from real time + real location: sunrise over
// Shibuya happens when it actually would. Accuracy ~0.01° over 1950-2050,
// far below what lighting needs.

#include <cstdint>

namespace rj::env {

struct SunPosition {
  double elevation_deg;  // above horizon, with approximate atmospheric refraction
  double azimuth_deg;    // clockwise from true north
  double declination_deg;
  double equation_of_time_min;
};

SunPosition sunPosition(int64_t unix_utc, double lat_deg, double lon_deg);

}  // namespace rj::env
