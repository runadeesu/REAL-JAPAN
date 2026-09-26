#include "rj/sim/schedule.hpp"
#include "rj_test.hpp"

using namespace rj::sim;

namespace {
PlaceRef place(const char* id, double lat, double lon) { return {id, {lat, lon}}; }

Npc officeWorker() {
  ResidentSpec spec{42, 7, place("home-1", 35.6555, 139.6985), place("office-1", 35.6600, 139.7030),
                    "office_worker"};
  Npc n = generateResident(spec);
  n.hobbies = {Hobby::Skateboard, Hobby::VideoGames};
  return n;
}

DayContext ctxFor(CivilDate d, WeatherKind w = WeatherKind::Clear) {
  DayContext c;
  c.date = d;
  c.weather = w;
  c.nearby_shop = place("konbini-1", 35.6570, 139.7000);
  c.leisure_spot = place("park-1", 35.6620, 139.6990);
  c.clinic = place("clinic-1", 35.6580, 139.7040);
  return c;
}

bool wellFormed(const DailyPlan& p, bool sleep_first = true) {
  if (p.activities.empty() || p.activities.front().start_min != 0) return false;
  if (p.activities.back().end_min != 1440) return false;
  for (size_t i = 0; i < p.activities.size(); ++i) {
    if (p.activities[i].end_min <= p.activities[i].start_min) return false;
    if (i && p.activities[i].start_min != p.activities[i - 1].end_min) return false;
  }
  return !sleep_first || p.activities.front().type == ActivityType::Sleep;
}
}  // namespace

RJ_TEST(resident_generation_is_deterministic) {
  const Npc a = officeWorker(), b = officeWorker();
  RJ_CHECK_EQ(a.fullName(), b.fullName());
  RJ_CHECK_EQ(a.age, b.age);
  RJ_CHECK(a.age >= 20 && a.age <= 64);
  RJ_CHECK_EQ(allOccupations().size(), 51u);
  RJ_CHECK(occupationById("train_driver") != nullptr);
  RJ_CHECK(occupationById("no_such_job") == nullptr);
}

RJ_TEST(weekday_office_worker_commutes_and_works) {
  const Npc n = officeWorker();
  const DailyPlan p = planDay(n, ctxFor({2026, 9, 24}));  // Thursday
  RJ_CHECK(wellFormed(p));
  RJ_CHECK(p.contains(ActivityType::Work));
  RJ_CHECK(p.contains(ActivityType::Commute));
  const Activity* at10 = p.at(10 * 60);
  RJ_CHECK(at10 && at10->type == ActivityType::Work && at10->place.building_id == "office-1");
  const Activity* at3 = p.at(3 * 60);
  RJ_CHECK(at3 && at3->type == ActivityType::Sleep && at3->place.building_id == "home-1");
}

RJ_TEST(holiday_and_weekend_differ_from_workday) {
  const Npc n = officeWorker();
  RJ_CHECK(!planDay(n, ctxFor({2026, 9, 22})).contains(ActivityType::Work));  // 国民の休日
  RJ_CHECK(!planDay(n, ctxFor({2026, 9, 26})).contains(ActivityType::Work));  // Saturday
  // Shop clerks work holidays (except their personal days off).
  ResidentSpec spec{77, 7, place("home-2", 35.6555, 139.6985), place("shop-1", 35.6590, 139.7005), "shop_clerk"};
  const Npc clerk = generateResident(spec);
  int worked = 0;
  for (int d = 0; d < 7; ++d) worked += worksOn(clerk, addDays({2026, 9, 21}, d)) ? 1 : 0;
  RJ_CHECK_EQ(worked, 5);
}

RJ_TEST(sick_npc_stays_home) {
  Npc n = officeWorker();
  n.health.sick = true;
  n.health.sickness_severity = 0.8f;
  const DailyPlan p = planDay(n, ctxFor({2026, 9, 24}));
  RJ_CHECK(wellFormed(p));
  RJ_CHECK(!p.contains(ActivityType::Work));
  RJ_CHECK(p.contains(ActivityType::SickRest));
  RJ_CHECK(p.contains(ActivityType::MedicalVisit));
}

RJ_TEST(rain_cancels_outdoor_hobbies) {
  Npc n = officeWorker();
  n.hobbies = {Hobby::Skateboard, Hobby::Surfing};  // both outdoor
  for (int d = 0; d < 20; ++d) {
    const DailyPlan p = planDay(n, ctxFor(addDays({2026, 9, 26}, 7 * d), WeatherKind::Rain));
    for (const auto& a : p.activities) RJ_CHECK(!(a.type == ActivityType::Hobby && a.hobby && hobbyIsOutdoor(*a.hobby)));
  }
  // In clear weather the same NPC does go skating on some days off.
  bool skated = false;
  for (int d = 0; d < 20; ++d) skated |= planDay(n, ctxFor(addDays({2026, 9, 26}, 7 * d))).contains(ActivityType::Hobby);
  RJ_CHECK(skated);
}

RJ_TEST(plans_vary_by_day_but_are_reproducible) {
  const Npc n = officeWorker();
  const DailyPlan a1 = planDay(n, ctxFor({2026, 9, 24}));
  const DailyPlan a2 = planDay(n, ctxFor({2026, 9, 24}));
  RJ_CHECK_EQ(a1.activities.size(), a2.activities.size());
  RJ_CHECK_EQ(a1.activities[1].start_min, a2.activities[1].start_min);
  int distinct_wake = 0, last = -1;
  for (int d = 0; d < 10; ++d) {
    const DailyPlan p = planDay(n, ctxFor(addDays({2026, 9, 28}, d)));
    RJ_CHECK(wellFormed(p));
    const int wake = p.activities.front().end_min;
    if (wake != last) ++distinct_wake;
    last = wake;
  }
  RJ_CHECK(distinct_wake >= 5);
}

RJ_TEST(night_shift_spans_midnight) {
  ResidentSpec spec{9001, 7, place("home-3", 35.6555, 139.6985), place("hospital-1", 35.6610, 139.6950), "nurse"};
  const Npc nurse = generateResident(spec);
  // Find two consecutive working days.
  CivilDate d{2026, 9, 21};
  while (!(worksOn(nurse, d) && worksOn(nurse, addDays(d, 1)))) d = addDays(d, 1);
  const DailyPlan p = planDay(nurse, ctxFor(addDays(d, 1)));
  RJ_CHECK(wellFormed(p, /*sleep_first=*/false));  // the day starts mid-shift
  RJ_CHECK(p.at(2 * 60)->type == ActivityType::Work);   // carried over from last night
  RJ_CHECK(p.at(23 * 60)->type == ActivityType::Work);  // tonight's shift
}

RJ_TEST(travel_estimate_modes) {
  TravelMode m;
  RJ_CHECK(estimateTravelMinutes(place("a", 35.658, 139.70), place("b", 35.659, 139.70), false, &m) > 0);
  RJ_CHECK(m == TravelMode::Walk);
  estimateTravelMinutes(place("a", 35.658, 139.70), place("c", 35.69, 139.70), false, &m);
  RJ_CHECK(m == TravelMode::Train);
  RJ_CHECK_EQ(estimateTravelMinutes(place("a", 35.658, 139.70), place("a", 35.658, 139.70), false, &m), 0);
}
