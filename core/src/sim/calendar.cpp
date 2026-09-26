#include "rj/sim/calendar.hpp"

#include <cmath>

namespace rj::sim {
namespace {

// n-th weekday (0=Sun) of a month, n >= 1.
int nthWeekday(int y, int m, int wd, int n) {
  const int first = weekday({y, m, 1});
  return 1 + (wd - first + 7) % 7 + 7 * (n - 1);
}

int vernalEquinoxDay(int y) {
  return static_cast<int>(std::floor(20.8431 + 0.242194 * (y - 1980) - std::floor((y - 1980) / 4.0)));
}
int autumnalEquinoxDay(int y) {
  return static_cast<int>(std::floor(23.2488 + 0.242194 * (y - 1980) - std::floor((y - 1980) / 4.0)));
}

// Statutory holidays only (no substitute / citizens' holidays).
std::optional<std::string_view> statutoryHoliday(const CivilDate& c) {
  const int y = c.y, m = c.m, d = c.d;
  switch (m) {
    case 1:
      if (d == 1) return "元日";
      if (d == nthWeekday(y, 1, 1, 2)) return "成人の日";
      break;
    case 2:
      if (d == 11) return "建国記念の日";
      if (y >= 2020 && d == 23) return "天皇誕生日";
      break;
    case 3:
      if (d == vernalEquinoxDay(y)) return "春分の日";
      break;
    case 4:
      if (d == 29) return "昭和の日";
      break;
    case 5:
      if (d == 3) return "憲法記念日";
      if (d == 4) return "みどりの日";
      if (d == 5) return "こどもの日";
      break;
    case 7:
      if (d == nthWeekday(y, 7, 1, 3)) return "海の日";
      break;
    case 8:
      if (d == 11) return "山の日";
      break;
    case 9:
      if (d == nthWeekday(y, 9, 1, 3)) return "敬老の日";
      if (d == autumnalEquinoxDay(y)) return "秋分の日";
      break;
    case 10:
      if (d == nthWeekday(y, 10, 1, 2)) return "スポーツの日";
      break;
    case 11:
      if (d == 3) return "文化の日";
      if (d == 23) return "勤労感謝の日";
      break;
    default:
      break;
  }
  return std::nullopt;
}

}  // namespace

int64_t daysFromCivil(const CivilDate& c) {
  // Howard Hinnant's algorithm.
  int y = c.y;
  const unsigned m = static_cast<unsigned>(c.m), d = static_cast<unsigned>(c.d);
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

CivilDate civilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d)};
}

int weekday(const CivilDate& d) {
  const int64_t z = daysFromCivil(d);
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

CivilDate addDays(const CivilDate& d, int n) { return civilFromDays(daysFromCivil(d) + n); }

std::optional<std::string_view> holidayName(const CivilDate& d) {
  if (auto s = statutoryHoliday(d)) return s;
  // 振替休日: when a statutory holiday falls on Sunday, the next day that is
  // not a statutory holiday becomes a holiday.
  for (int back = 1; back <= 7; ++back) {
    const CivilDate p = addDays(d, -back);
    if (!statutoryHoliday(p)) break;
    if (weekday(p) == 0) return "振替休日";
  }
  // 国民の休日: a non-Sunday day sandwiched between two statutory holidays.
  if (weekday(d) != 0 && statutoryHoliday(addDays(d, -1)) && statutoryHoliday(addDays(d, 1)))
    return "国民の休日";
  return std::nullopt;
}

DayType dayType(const CivilDate& d) {
  const int wd = weekday(d);
  if (wd == 0 || isHoliday(d)) return DayType::SundayOrHoliday;
  if (wd == 6) return DayType::Saturday;
  return DayType::Weekday;
}

Season season(const CivilDate& d) {
  if (d.m >= 3 && d.m <= 5) return Season::Spring;
  if (d.m >= 6 && d.m <= 8) return Season::Summer;
  if (d.m >= 9 && d.m <= 11) return Season::Autumn;
  return Season::Winter;
}

CivilDateTime GameClock::jst() const {
  const int64_t t = static_cast<int64_t>(std::floor(unix_)) + 9 * 3600;
  int64_t days = t / 86400;
  int64_t sod = t % 86400;
  if (sod < 0) {
    sod += 86400;
    --days;
  }
  CivilDateTime out;
  out.date = civilFromDays(days);
  out.hour = static_cast<int>(sod / 3600);
  out.minute = static_cast<int>((sod % 3600) / 60);
  out.second = static_cast<int>(sod % 60);
  return out;
}

int64_t GameClock::unixFromJst(const CivilDate& d, int hour, int minute, int second) {
  return daysFromCivil(d) * 86400 + hour * 3600 + minute * 60 + second - 9 * 3600;
}

}  // namespace rj::sim
