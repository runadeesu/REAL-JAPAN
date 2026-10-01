#include "game/weather.hpp"

#include <algorithm>
#include <cmath>

namespace rjc {

WeatherParams weatherTarget(WeatherKind k) {
  switch (k) {
    case WeatherKind::Clear: return {0.02f, 0.0f, 0.0f, 2.0f};
    case WeatherKind::Fair: return {0.30f, 0.0f, 0.0f, 3.0f};
    case WeatherKind::ThinCloud: return {0.60f, 0.0f, 0.05f, 3.0f};
    case WeatherKind::Overcast: return {0.92f, 0.0f, 0.10f, 4.0f};
    case WeatherKind::LightRain: return {0.95f, 1.5f, 0.20f, 4.0f};
    case WeatherKind::Rain: return {1.00f, 6.0f, 0.30f, 5.0f};
    case WeatherKind::HeavyRain: return {1.00f, 30.0f, 0.50f, 8.0f};
    case WeatherKind::Thunder: return {1.00f, 20.0f, 0.40f, 10.0f};
    case WeatherKind::Fog: return {0.75f, 0.0f, 1.00f, 1.0f};
    case WeatherKind::Windy: return {0.40f, 0.0f, 0.0f, 14.0f};
    case WeatherKind::Typhoon: return {1.00f, 35.0f, 0.45f, 21.0f};
    default: return {};
  }
}

const char* weatherKey(WeatherKind k) {
  static const char* keys[] = {"clear", "fair", "thin_cloud", "overcast", "light_rain", "rain", "heavy_rain", "thunder", "fog", "windy", "typhoon"};
  const int i = static_cast<int>(k);
  return (i >= 0 && i < static_cast<int>(WeatherKind::Count)) ? keys[i] : "fair";
}

bool parseWeather(const std::string& s, WeatherKind& out) {
  for (int i = 0; i < static_cast<int>(WeatherKind::Count); ++i)
    if (s == weatherKey(static_cast<WeatherKind>(i))) {
      out = static_cast<WeatherKind>(i);
      return true;
    }
  return false;
}

float WeatherSim::rand01() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ & 0xffffff) / 16777215.0f;
}

void WeatherSim::set(WeatherKind k, bool instant) {
  kind_ = k;
  until_change_s_ = 1200.0 + 3000.0 * rand01();
  if (instant) {
    cur_ = weatherTarget(k);
    if (cur_.rain_mm_h > 0.0f) wetness_ = std::min(1.0f, 0.5f + cur_.rain_mm_h / 20.0f);
  }
}

void WeatherSim::update(double game_dt_s, double real_dt_s, float sun_elevation_deg) {
  const float gdt = static_cast<float>(std::clamp(game_dt_s, 0.0, 600.0));
  if (auto_) {
    until_change_s_ -= gdt;
    if (until_change_s_ <= 0.0) {
      // Markov step: mostly neighbouring states (clear <-> cloud <-> rain), rare jumps.
      static const int next[][4] = {
          {1, 1, 2, 0}, {0, 2, 2, 3}, {1, 3, 3, 1}, {2, 4, 4, 2}, {3, 5, 3, 3},
          {4, 6, 4, 3}, {5, 5, 7, 5}, {6, 5, 5, 3}, {2, 3, 1, 1}, {1, 1, 2, 0}, {6, 9, 5, 6}};
      const int cur = static_cast<int>(kind_);
      int nk = next[cur][static_cast<int>(rand01() * 3.999f)];
      if (tsuyu() && rand01() < 0.45f) nk = rand01() < 0.35f ? 3 : rand01() < 0.6f ? 4 : 5;  // the rainy season: grey and wet
      if (wet_bias_ > 0.0f && nk < 5 && rand01() < wet_bias_) ++nk;  // (a wetter region: the change leans to cloud and rain)
      if (wet_bias_ < 0.0f && nk > 0 && nk <= 5 && rand01() < -wet_bias_) --nk;
      if (typhoonSeason() && kind_ != WeatherKind::Typhoon && rand01() < 0.03f * typhoon_mul_) nk = static_cast<int>(WeatherKind::Typhoon);
      set(static_cast<WeatherKind>(nk), false);
      if (kind_ == WeatherKind::Typhoon) until_change_s_ = 3.0 * 3600.0 + 3.0 * 3600.0 * rand01();  // (it passes in a few hours)
    }
  }
  // Smooth approach to the state's parameters (~8 game minutes).
  const WeatherParams t = weatherTarget(kind_);
  const float k = 1.0f - std::exp(-gdt / 480.0f);
  cur_.cloud_cover += (t.cloud_cover - cur_.cloud_cover) * k;
  cur_.rain_mm_h += (t.rain_mm_h - cur_.rain_mm_h) * k;
  cur_.fog += (t.fog - cur_.fog) * k;
  cur_.wind_ms += (t.wind_ms - cur_.wind_ms) * k;
  // Wetness: rain wets quickly, sun + wind dry slowly (hours).
  const float sun = std::clamp(sun_elevation_deg / 40.0f, 0.0f, 1.0f) * (1.0f - cur_.cloud_cover);
  const float wet_in = cur_.rain_mm_h > 0.05f ? std::min(1.0f, cur_.rain_mm_h / 4.0f) * gdt / 300.0f : 0.0f;
  const float dry = (0.15f + 0.8f * sun + 0.04f * cur_.wind_ms) * gdt / 3600.0f;
  wetness_ = std::clamp(wetness_ + wet_in - (cur_.rain_mm_h > 0.05f ? 0.0f : dry), 0.0f, 1.0f);
  // Clouds drift with the wind (real time so the sky visibly moves).
  const float rdt = static_cast<float>(std::clamp(real_dt_s, 0.0, 0.2));
  cloud_x_ += std::sin(wind_dir_) * cur_.wind_ms * 3.0f * rdt;
  cloud_y_ += std::cos(wind_dir_) * cur_.wind_ms * 3.0f * rdt;
  // Lightning flashes in thunderstorms.
  lightning_ = std::max(0.0f, lightning_ - rdt * 6.0f);
  if (kind_ == WeatherKind::Thunder) {
    next_flash_s_ -= rdt;
    if (next_flash_s_ <= 0.0) {
      lightning_ = 1.0f;
      next_flash_s_ = 6.0 + 22.0 * rand01();
    }
  }
}

}  // namespace rjc
