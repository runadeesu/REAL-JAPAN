#pragma once
// Local weather simulation for the slice (game-side model; NOT a real forecast feed).
// States change on a Markov chain every 20-70 game minutes with smooth transitions.
// Wetness accumulates with rain and dries with sun and wind; it drives wet roads,
// puddles and reflections in the renderer. Snow / typhoon / tsuyu are TODO but the
// state structure (parameters, not hard-coded effects) is built for them.

#include <cstdint>
#include <string>

namespace rjc {

enum class WeatherKind : int {
  Clear = 0,   // 快晴
  Fair,        // 晴れ
  ThinCloud,   // 薄曇り
  Overcast,    // 曇り
  LightRain,   // 小雨
  Rain,        // 雨
  HeavyRain,   // 豪雨
  Thunder,     // 雷雨
  Fog,         // 霧
  Windy,       // 強風
  Count
};

struct WeatherParams {
  float cloud_cover = 0.1f;  // 0..1
  float rain_mm_h = 0.0f;
  float fog = 0.0f;          // 0..1 (1 = ~150 m visibility)
  float wind_ms = 2.0f;
};

WeatherParams weatherTarget(WeatherKind k);
const char* weatherKey(WeatherKind k);  // i18n key suffix, e.g. "clear"
bool parseWeather(const std::string& s, WeatherKind& out);

class WeatherSim {
 public:
  void set(WeatherKind k, bool instant);
  void setAuto(bool on) { auto_ = on; }
  void update(double game_dt_s, double real_dt_s, float sun_elevation_deg);

  WeatherKind kind() const { return kind_; }
  const WeatherParams& now() const { return cur_; }
  float wetness() const { return wetness_; }
  float lightning() const { return lightning_; }
  float windDirRad() const { return wind_dir_; }
  float cloudOffsetX() const { return cloud_x_; }
  float cloudOffsetY() const { return cloud_y_; }

 private:
  WeatherKind kind_ = WeatherKind::Fair;
  WeatherParams cur_ = weatherTarget(WeatherKind::Fair);
  bool auto_ = true;
  double until_change_s_ = 2400.0;
  float wetness_ = 0.0f;
  float lightning_ = 0.0f;
  double next_flash_s_ = 20.0;
  float wind_dir_ = 2.4f;  // radians, direction the wind blows towards (compass)
  float cloud_x_ = 0.0f, cloud_y_ = 0.0f;
  uint32_t rng_ = 20260926u;
  float rand01();
};

}  // namespace rjc
