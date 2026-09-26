#include "rj/sim/calendar.hpp"
#include "rj_test.hpp"

using namespace rj::sim;

RJ_TEST(civil_days_roundtrip_and_weekday) {
  RJ_CHECK_EQ(daysFromCivil({1970, 1, 1}), 0);
  RJ_CHECK(civilFromDays(daysFromCivil({2026, 9, 26})) == (CivilDate{2026, 9, 26}));
  RJ_CHECK_EQ(weekday({1970, 1, 1}), 4);   // Thursday
  RJ_CHECK_EQ(weekday({2026, 1, 1}), 4);   // Thursday
  RJ_CHECK_EQ(weekday({2026, 9, 26}), 6);  // Saturday
  RJ_CHECK(addDays({2026, 12, 31}, 1) == (CivilDate{2027, 1, 1}));
  RJ_CHECK(addDays({2028, 2, 28}, 1) == (CivilDate{2028, 2, 29}));  // leap year
}

RJ_TEST(japanese_holidays_2026) {
  RJ_CHECK_EQ(*holidayName({2026, 1, 1}), std::string_view("元日"));
  RJ_CHECK_EQ(*holidayName({2026, 1, 12}), std::string_view("成人の日"));
  RJ_CHECK_EQ(*holidayName({2026, 2, 23}), std::string_view("天皇誕生日"));
  RJ_CHECK_EQ(*holidayName({2026, 3, 20}), std::string_view("春分の日"));
  // 5/3 (Sun) Constitution Day -> substitute holiday on Wed 5/6.
  RJ_CHECK_EQ(*holidayName({2026, 5, 6}), std::string_view("振替休日"));
  RJ_CHECK_EQ(*holidayName({2026, 7, 20}), std::string_view("海の日"));
  // Silver Week 2026: 9/21 敬老の日, 9/22 国民の休日, 9/23 秋分の日.
  RJ_CHECK_EQ(*holidayName({2026, 9, 21}), std::string_view("敬老の日"));
  RJ_CHECK_EQ(*holidayName({2026, 9, 22}), std::string_view("国民の休日"));
  RJ_CHECK_EQ(*holidayName({2026, 9, 23}), std::string_view("秋分の日"));
  RJ_CHECK_EQ(*holidayName({2026, 10, 12}), std::string_view("スポーツの日"));
  RJ_CHECK(!isHoliday({2026, 9, 24}));
  RJ_CHECK(!isHoliday({2026, 5, 7}));
  RJ_CHECK(dayType({2026, 9, 22}) == DayType::SundayOrHoliday);
  RJ_CHECK(dayType({2026, 9, 26}) == DayType::Saturday);
  RJ_CHECK(dayType({2026, 9, 24}) == DayType::Weekday);
}

RJ_TEST(seasons_and_clock) {
  RJ_CHECK(season({2026, 4, 1}) == Season::Spring);
  RJ_CHECK(season({2026, 8, 1}) == Season::Summer);
  RJ_CHECK(season({2026, 11, 30}) == Season::Autumn);
  RJ_CHECK(season({2027, 1, 15}) == Season::Winter);
  const int64_t t = GameClock::unixFromJst({2026, 9, 26}, 8, 15);
  GameClock c(t, 60.0);
  RJ_CHECK_EQ(c.jst().hour, 8);
  RJ_CHECK_EQ(c.jst().minute, 15);
  c.advanceReal(60.0);  // one real minute = one game hour at x60
  RJ_CHECK_EQ(c.jst().hour, 9);
  c.advanceGame(16 * 3600);
  RJ_CHECK(c.jst().date == (CivilDate{2026, 9, 27}));
  RJ_CHECK_EQ(c.jst().hour, 1);
}
