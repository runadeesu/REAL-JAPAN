#include "rj/sim/schedule.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace rj::sim {
namespace {

constexpr int kDay = 24 * 60;

constexpr std::array<std::string_view, static_cast<size_t>(ActivityType::kCount)> kActivityJa{
    "睡眠", "身支度", "朝食", "移動", "仕事", "学校", "昼食", "買い物", "趣味", "友人と過ごす",
    "夕食", "家で過ごす", "通院", "療養", "散歩"};

struct ShiftTimes {
  int start, end, meal;  // minutes of day; meal = start of meal break (-1 none)
};

ShiftTimes shiftTimes(ShiftPattern s, Rng& r) {
  switch (s) {
    case ShiftPattern::Day: return {9 * 60, 18 * 60, 12 * 60};
    case ShiftPattern::Early: return {6 * 60 + 30, 15 * 60 + 30, 11 * 60 + 30};
    case ShiftPattern::Late: return {13 * 60, 22 * 60, 17 * 60 + 30};
    case ShiftPattern::Night: return {22 * 60, 24 * 60, -1};
    case ShiftPattern::School: return {8 * 60 + 20, 15 * 60 + 30, -1};
    case ShiftPattern::Flexible: {
      const int start = std::clamp(static_cast<int>(r.normal(10 * 60, 50)), 7 * 60, 13 * 60);
      return {start, start + 8 * 60, start + 3 * 60};
    }
    case ShiftPattern::None: break;
  }
  return {0, 0, -1};
}

class Builder {
 public:
  explicit Builder(DailyPlan& p) : p_(p) {}
  int cursor() const { return cur_; }
  // Append an activity of `minutes` starting at the cursor (clamped to the day).
  void add(ActivityType t, int minutes, const PlaceRef& place, TravelMode mode = TravelMode::None,
           std::optional<Hobby> hobby = std::nullopt) {
    if (minutes <= 0 || cur_ >= kDay) return;
    const int end = std::min(kDay, cur_ + minutes);
    push(t, end, place, mode, hobby);
  }
  // Extend with `t` until an absolute minute (no-op if already past it).
  void until(ActivityType t, int end_min, const PlaceRef& place) {
    end_min = std::min(end_min, kDay);
    if (end_min <= cur_) return;
    push(t, end_min, place, TravelMode::None, std::nullopt);
  }
  void travel(const PlaceRef& from, const PlaceRef& to, bool has_car) {
    TravelMode mode = TravelMode::None;
    const int m = estimateTravelMinutes(from, to, has_car, &mode);
    if (m > 0) add(ActivityType::Commute, m, to, mode);
  }

 private:
  void push(ActivityType t, int end, const PlaceRef& place, TravelMode mode, std::optional<Hobby> h) {
    auto& a = p_.activities;
    if (!a.empty() && a.back().type == t && a.back().place.building_id == place.building_id &&
        t != ActivityType::Commute) {
      a.back().end_min = end;  // merge adjacent identical activities
    } else {
      a.push_back({cur_, end, t, place, mode, h});
    }
    cur_ = end;
  }
  DailyPlan& p_;
  int cur_ = 0;
};

std::optional<Hobby> pickHobby(const Npc& n, WeatherKind w, Rng& r) {
  if (n.hobbies.empty()) return std::nullopt;
  std::vector<Hobby> ok;
  for (Hobby h : n.hobbies)
    if (!(isBadWeather(w) && hobbyIsOutdoor(h))) ok.push_back(h);
  if (ok.empty()) return std::nullopt;
  return ok[r.next() % ok.size()];
}

int bedtimeFor(const Npc& n, Rng& r, int wake_tomorrow) {
  return wake_tomorrow + kDay - n.sleep_need_min + static_cast<int>(r.normal(0, 20));
}

void finishEvening(Builder& b, const Npc& n, Rng& r, int wake_tomorrow) {
  if (b.cursor() < 21 * 60 + 30) b.add(ActivityType::Dinner, r.range(25, 45), n.home);
  b.until(ActivityType::HomeLeisure, bedtimeFor(n, r, wake_tomorrow), n.home);
  b.until(ActivityType::Sleep, kDay, n.home);
}

void planSickDay(Builder& b, const Npc& n, const DayContext& ctx, Rng& r) {
  const bool has_car = n.vehicle.has_value();
  b.until(ActivityType::Sleep, n.wake_minute + 60, n.home);
  if (n.health.sickness_severity > 0.5f && ctx.clinic.valid()) {
    b.until(ActivityType::SickRest, 9 * 60 + 30, n.home);
    b.travel(n.home, ctx.clinic, has_car);
    b.add(ActivityType::MedicalVisit, r.range(60, 120), ctx.clinic);
    b.travel(ctx.clinic, n.home, has_car);
  }
  b.until(ActivityType::SickRest, 21 * 60 + static_cast<int>(r.normal(0, 30)), n.home);
  b.until(ActivityType::Sleep, kDay, n.home);
}

void planWorkday(Builder& b, const Npc& n, const OccupationDef& occ, const DayContext& ctx, Rng& r,
                 bool night_carry_over) {
  const bool has_car = n.vehicle.has_value();
  const PlaceRef& dest = occ.shift == ShiftPattern::School ? n.school : n.work;
  const bool remote = occ.workplace == WorkplaceKind::Home || !dest.valid();
  const PlaceRef& site = remote ? n.home : dest;
  const ShiftTimes st = shiftTimes(occ.shift, r);

  if (occ.shift == ShiftPattern::Night) {
    if (night_carry_over) {
      b.until(ActivityType::Work, 7 * 60, site);
      if (!remote) b.travel(site, n.home, has_car);
      b.add(ActivityType::Breakfast, 20, n.home);
      b.add(ActivityType::Sleep, n.sleep_need_min, n.home);
    } else {
      b.until(ActivityType::Sleep, n.wake_minute + 90, n.home);
    }
    b.add(ActivityType::MorningRoutine, 30, n.home);
    if (ctx.nearby_shop.valid() && r.chance(0.4)) {
      b.travel(n.home, ctx.nearby_shop, has_car);
      b.add(ActivityType::Shopping, r.range(15, 35), ctx.nearby_shop);
      b.travel(ctx.nearby_shop, n.home, has_car);
    }
    int travel = remote ? 0 : estimateTravelMinutes(n.home, site, has_car, nullptr);
    b.until(ActivityType::HomeLeisure, 19 * 60, n.home);
    b.add(ActivityType::Dinner, 40, n.home);
    b.until(ActivityType::HomeLeisure, st.start - travel - 10, n.home);
    if (!remote) b.travel(n.home, site, has_car);
    b.until(ActivityType::Work, kDay, site);
    return;
  }

  const int travel = remote ? 0 : estimateTravelMinutes(n.home, site, has_car, nullptr);
  const int buffer = static_cast<int>(5 + 15 * n.personality.conscientiousness);
  const int leave = st.start - travel - buffer;
  const int routine = r.range(25, 40);
  const bool breakfast = r.uniform() > 0.25 * (1.0 - n.personality.conscientiousness);
  int wake = std::min(n.wake_minute, leave - routine - (breakfast ? 15 : 0));
  wake += static_cast<int>(r.normal(0, 8));
  wake = std::max(wake, 4 * 60);

  b.until(ActivityType::Sleep, wake, n.home);
  b.add(ActivityType::MorningRoutine, routine, n.home);
  if (breakfast) b.add(ActivityType::Breakfast, 15, n.home);
  b.until(ActivityType::HomeLeisure, leave, n.home);
  if (!remote) b.travel(n.home, site, has_car);

  const ActivityType work_type =
      occ.shift == ShiftPattern::School ? ActivityType::School : ActivityType::Work;
  int end = st.end;
  if (occ.workplace == WorkplaceKind::Office)
    end += std::max(0, static_cast<int>(r.normal(25, 35)));  // overtime varies day to day
  if (st.meal >= 0) {
    b.until(work_type, st.meal, site);
    b.add(ActivityType::Lunch, r.range(40, 60), site);
  }
  b.until(work_type, end, site);

  PlaceRef here = site;
  if (ctx.friend_invite && ctx.meetup_place.valid() && b.cursor() < 20 * 60) {
    b.travel(here, ctx.meetup_place, has_car);
    b.add(ActivityType::Socialize, r.range(90, 150), ctx.meetup_place);
    here = ctx.meetup_place;
  } else if (auto h = pickHobby(n, ctx.weather, r); h && r.chance(0.25) && b.cursor() < 19 * 60) {
    const PlaceRef& spot = ctx.leisure_spot.valid() ? ctx.leisure_spot : n.home;
    b.travel(here, spot, has_car);
    b.add(ActivityType::Hobby, r.range(60, 120), spot, TravelMode::None, h);
    here = spot;
  }
  if (ctx.nearby_shop.valid() && r.chance(0.35) && b.cursor() < 22 * 60) {
    b.travel(here, ctx.nearby_shop, has_car);
    b.add(ActivityType::Shopping, r.range(10, 35), ctx.nearby_shop);
    here = ctx.nearby_shop;
  }
  b.travel(here, n.home, has_car);
  finishEvening(b, n, r, n.wake_minute);
}

void planDayOff(Builder& b, const Npc& n, const DayContext& ctx, Rng& r) {
  const bool has_car = n.vehicle.has_value();
  const bool retired = n.occupation_id == "retired";
  const int wake = n.wake_minute + (retired ? 0 : r.range(45, 150));
  b.until(ActivityType::Sleep, wake, n.home);
  b.add(ActivityType::MorningRoutine, r.range(20, 45), n.home);
  b.add(ActivityType::Breakfast, r.range(15, 35), n.home);

  if (retired && !isBadWeather(ctx.weather) && ctx.leisure_spot.valid() && r.chance(0.7)) {
    b.travel(n.home, ctx.leisure_spot, has_car);
    b.add(ActivityType::Walk, r.range(30, 70), ctx.leisure_spot);
    b.travel(ctx.leisure_spot, n.home, has_car);
  }
  b.until(ActivityType::HomeLeisure, b.cursor() + r.range(30, 120), n.home);

  PlaceRef here = n.home;
  if (ctx.friend_invite && ctx.meetup_place.valid()) {
    b.travel(here, ctx.meetup_place, has_car);
    b.add(ActivityType::Socialize, r.range(150, 300), ctx.meetup_place);
    here = ctx.meetup_place;
  } else if (auto h = pickHobby(n, ctx.weather, r)) {
    const bool go_out = (hobbyIsOutdoor(*h) || r.chance(0.4)) && ctx.leisure_spot.valid();
    const PlaceRef& spot = go_out ? ctx.leisure_spot : n.home;
    b.travel(here, spot, has_car);
    b.add(ActivityType::Hobby, r.range(90, 240), spot, TravelMode::None, h);
    here = spot;
  }
  if (ctx.nearby_shop.valid() && r.chance(0.55)) {
    b.travel(here, ctx.nearby_shop, has_car);
    b.add(ActivityType::Shopping, r.range(20, 60), ctx.nearby_shop);
    here = ctx.nearby_shop;
  }
  b.travel(here, n.home, has_car);
  finishEvening(b, n, r, n.wake_minute);
}

}  // namespace

std::string_view activityNameJa(ActivityType t) {
  const auto i = static_cast<size_t>(t);
  return i < kActivityJa.size() ? kActivityJa[i] : "?";
}

const Activity* DailyPlan::at(int minute) const {
  for (const auto& a : activities)
    if (minute >= a.start_min && minute < a.end_min) return &a;
  return nullptr;
}

bool DailyPlan::contains(ActivityType t) const {
  return std::any_of(activities.begin(), activities.end(), [&](const Activity& a) { return a.type == t; });
}

bool worksOn(const Npc& npc, const CivilDate& date) {
  const OccupationDef* occ = occupationById(npc.occupation_id);
  if (!occ || occ->shift == ShiftPattern::None) return false;
  const int wd = weekday(date);
  if (!((occ->workdays >> wd) & 1)) return false;
  if (isHoliday(date) && !occ->works_on_holidays) return false;
  if (occ->workdays == kEveryDay) {
    // 7-day rotations: two personal days off per week derived from the seed.
    const int off_a = static_cast<int>(npc.seed % 7);
    const int off_b = (off_a + 3) % 7;
    if (wd == off_a || wd == off_b) return false;
  }
  return true;
}

int estimateTravelMinutes(const PlaceRef& from, const PlaceRef& to, bool has_car, TravelMode* mode_out) {
  if (!from.valid() || !to.valid() || from.building_id == to.building_id) {
    if (mode_out) *mode_out = TravelMode::None;
    return 0;
  }
  const double d = geo::approxDistanceM(from.pos, to.pos) * 1.3;  // detour factor
  TravelMode mode;
  double minutes;
  if (d < 1500.0) {
    mode = TravelMode::Walk;
    minutes = d / 1.3 / 60.0;
  } else if (has_car && d < 8000.0) {
    mode = TravelMode::Car;
    minutes = 5.0 + d / 6.0 / 60.0;
  } else {
    mode = TravelMode::Train;
    minutes = 12.0 + d / 9.0 / 60.0;  // walk to station + wait + ride
  }
  if (mode_out) *mode_out = mode;
  return std::max(1, static_cast<int>(std::lround(minutes)));
}

DailyPlan planDay(const Npc& npc, const DayContext& ctx) {
  DailyPlan plan;
  plan.date = ctx.date;
  Rng r(hashCombine(npc.seed, static_cast<uint64_t>(daysFromCivil(ctx.date))));
  Builder b(plan);
  const OccupationDef* occ = occupationById(npc.occupation_id);

  if (npc.health.sick) {
    planSickDay(b, npc, ctx, r);
  } else if (occ && worksOn(npc, ctx.date)) {
    const bool carry = occ->shift == ShiftPattern::Night && worksOn(npc, addDays(ctx.date, -1));
    planWorkday(b, npc, *occ, ctx, r, carry);
  } else if (occ && occ->shift == ShiftPattern::Night && worksOn(npc, addDays(ctx.date, -1))) {
    // Coming off a night shift: finish it, sleep, then a short day off.
    b.until(ActivityType::Work, 7 * 60, npc.work.valid() ? npc.work : npc.home);
    b.travel(npc.work, npc.home, npc.vehicle.has_value());
    b.add(ActivityType::Sleep, npc.sleep_need_min, npc.home);
    finishEvening(b, npc, r, npc.wake_minute);
  } else {
    planDayOff(b, npc, ctx, r);
  }
  b.until(ActivityType::Sleep, kDay, npc.home);
  return plan;
}

}  // namespace rj::sim
