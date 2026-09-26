#include "rj/env/solar.hpp"
#include "rj/sim/calendar.hpp"
#include "rj_test.hpp"

using rj::env::sunPosition;
using rj::sim::GameClock;

RJ_TEST(sun_over_shibuya) {
  const double lat = 35.658, lon = 139.7016;
  // Summer solstice, local solar noon ~11:43 JST: elevation ~ 90 - 35.66 + 23.44 = 77.8°.
  auto noon = sunPosition(GameClock::unixFromJst({2026, 6, 21}, 11, 43), lat, lon);
  RJ_CHECK_NEAR(noon.elevation_deg, 77.8, 0.4);
  RJ_CHECK_NEAR(noon.declination_deg, 23.44, 0.05);
  // Winter solstice noon: ~30.9°.
  auto winter = sunPosition(GameClock::unixFromJst({2026, 12, 21}, 11, 38), lat, lon);
  RJ_CHECK_NEAR(winter.elevation_deg, 30.9, 0.4);
  RJ_CHECK(winter.azimuth_deg > 170.0 && winter.azimuth_deg < 190.0);
  // Tokyo sunrise on 6/21 is ~04:25 JST.
  RJ_CHECK(sunPosition(GameClock::unixFromJst({2026, 6, 21}, 4, 0), lat, lon).elevation_deg < 0.0);
  RJ_CHECK(sunPosition(GameClock::unixFromJst({2026, 6, 21}, 5, 0), lat, lon).elevation_deg > 0.0);
  // Midnight is dark; morning sun is in the east.
  RJ_CHECK(sunPosition(GameClock::unixFromJst({2026, 9, 26}, 0, 0), lat, lon).elevation_deg < -30.0);
  auto morning = sunPosition(GameClock::unixFromJst({2026, 9, 26}, 8, 0), lat, lon);
  RJ_CHECK(morning.azimuth_deg > 80.0 && morning.azimuth_deg < 130.0);
}
