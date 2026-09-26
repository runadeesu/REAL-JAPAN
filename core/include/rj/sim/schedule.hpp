#pragma once
// Daily plans. Every NPC gets a full 24h plan per day, generated
// deterministically from (npc.seed, date) plus the day's context (weekday /
// holiday, weather, health, social invitations). Plans differ day to day and
// never repeat exactly.
//
// Travel durations are STRAIGHT-LINE ESTIMATES for now; they are replaced by
// road / rail routing once Phase 4 (traffic) and Phase 5 (railway) land.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "rj/sim/calendar.hpp"
#include "rj/sim/npc.hpp"

namespace rj::sim {

enum class ActivityType : uint8_t {
  Sleep, MorningRoutine, Breakfast, Commute, Work, School, Lunch, Shopping, Hobby, Socialize,
  Dinner, HomeLeisure, MedicalVisit, SickRest, Walk, kCount
};
std::string_view activityNameJa(ActivityType t);

enum class TravelMode : uint8_t { None, Walk, Bicycle, Train, Car };

enum class WeatherKind : uint8_t { Clear, Cloudy, Rain, HeavyRain, Snow, Storm };
inline bool isBadWeather(WeatherKind w) { return w >= WeatherKind::Rain; }

struct Activity {
  int start_min = 0;  // minute of day, inclusive
  int end_min = 0;    // exclusive
  ActivityType type = ActivityType::Sleep;
  PlaceRef place;
  TravelMode mode = TravelMode::None;
  std::optional<Hobby> hobby;
};

struct DayContext {
  CivilDate date;
  WeatherKind weather = WeatherKind::Clear;
  bool friend_invite = false;
  PlaceRef meetup_place;   // where friends meet (if invited)
  PlaceRef nearby_shop;    // e.g. convenience store / supermarket near home or work
  PlaceRef leisure_spot;   // park, venue, arcade ...
  PlaceRef clinic;
};

struct DailyPlan {
  CivilDate date;
  std::vector<Activity> activities;  // contiguous, covering [0, 1440)
  const Activity* at(int minute_of_day) const;
  bool contains(ActivityType t) const;
};

// Whether the occupation works on this date for this NPC (handles weekly
// masks, holidays and personal days off for 7-day rotations).
bool worksOn(const Npc& npc, const CivilDate& date);

int estimateTravelMinutes(const PlaceRef& from, const PlaceRef& to, bool has_car, TravelMode* mode_out);

DailyPlan planDay(const Npc& npc, const DayContext& ctx);

}  // namespace rj::sim
