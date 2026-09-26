#pragma once
// Civil calendar in JST, Japanese national holidays, seasons and the game clock.
//
// Holidays follow 国民の祝日に関する法律 as amended through 2018 (Emperor's
// Birthday 2/23 from 2020, Sports Day naming from 2020). Vernal/autumnal
// equinox days use the standard prediction formula valid 1980-2099; the
// official dates are announced each February by the National Astronomical
// Observatory (官報). One-off exceptions (e.g. 2019 enthronement, 2020/2021
// Olympic moves) are not modelled; the game calendar starts in 2022.

#include <cstdint>
#include <optional>
#include <string_view>

namespace rj::sim {

struct CivilDate {
  int y = 2026, m = 1, d = 1;
  auto operator<=>(const CivilDate&) const = default;
};

struct CivilDateTime {
  CivilDate date;
  int hour = 0, minute = 0, second = 0;
  int minuteOfDay() const { return hour * 60 + minute; }
};

enum class DayType : uint8_t { Weekday, Saturday, SundayOrHoliday };
enum class Season : uint8_t { Spring, Summer, Autumn, Winter };

int64_t daysFromCivil(const CivilDate& d);  // days since 1970-01-01
CivilDate civilFromDays(int64_t days);
int weekday(const CivilDate& d);  // 0 = Sunday .. 6 = Saturday
CivilDate addDays(const CivilDate& d, int n);

// Name of the national holiday (祝日, 振替休日, 国民の休日) or nullopt.
std::optional<std::string_view> holidayName(const CivilDate& d);
inline bool isHoliday(const CivilDate& d) { return holidayName(d).has_value(); }
DayType dayType(const CivilDate& d);
// Meteorological seasons as used by JMA: MAM, JJA, SON, DJF.
Season season(const CivilDate& d);

// Game clock: authoritative time is UTC seconds; presentation is JST (UTC+9).
class GameClock {
 public:
  explicit GameClock(int64_t start_unix_utc, double time_scale = 30.0)
      : unix_(static_cast<double>(start_unix_utc)), scale_(time_scale) {}

  void advanceReal(double real_seconds) { unix_ += real_seconds * scale_; }
  void advanceGame(double game_seconds) { unix_ += game_seconds; }
  void setTimeScale(double s) { scale_ = s; }

  int64_t unixUtc() const { return static_cast<int64_t>(unix_); }
  double timeScale() const { return scale_; }
  CivilDateTime jst() const;

  static int64_t unixFromJst(const CivilDate& d, int hour, int minute, int second = 0);

 private:
  double unix_;
  double scale_;
};

}  // namespace rj::sim
