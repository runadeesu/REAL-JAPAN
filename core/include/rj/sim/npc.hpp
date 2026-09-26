#pragma once
// NPCs are residents, not spawner props. Each has an identity, home,
// workplace/school, income, relationships, tastes, body state and a daily
// plan. All NPCs are fictional people living in the real-geography world;
// they never impersonate real individuals.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rj/geo/ellipsoid.hpp"
#include "rj/sim/rng.hpp"

namespace rj::sim {

using EntityId = uint64_t;

enum class Hobby : uint8_t {
  VideoGames, Music, Instrument, Band, LiveMusic, Movies, Anime, Manga, Photography, Camera,
  Cars, Motorcycles, Cycling, Skateboard, BMX, FreestyleScooter, InlineSkate, Surfing, Skiing,
  Snowboarding, Camping, Hiking, Fishing, Travel, Railways, Aviation, Fashion, Cooking, Karaoke,
  Sports, kCount
};
std::string_view hobbyNameJa(Hobby h);
bool hobbyIsOutdoor(Hobby h);

enum class ShiftPattern : uint8_t { Day, Early, Late, Night, Flexible, School, None };

// Workdays bitmask, bit 0 = Sunday .. bit 6 = Saturday.
inline constexpr uint8_t kMonFri = 0b0111110;
inline constexpr uint8_t kMonSat = 0b1111110;
inline constexpr uint8_t kEveryDay = 0b1111111;

enum class WorkplaceKind : uint8_t {
  Office, Shop, Restaurant, Hospital, School, University, Station, Vehicle, Factory,
  ConstructionSite, Farm, Port, Airport, Studio, Venue, Home, Outdoor, PublicOffice, None
};

struct OccupationDef {
  std::string_view id;
  std::string_view name_ja;
  ShiftPattern shift;
  uint8_t workdays;
  bool works_on_holidays;  // 祝日も勤務 (shops, transport, hospitals ...)
  WorkplaceKind workplace;
  // GAME-BALANCE placeholder (JPY/month). Not a statistical claim; calibration
  // against 賃金構造基本統計調査 (e-Stat) is TODO.
  int64_t monthly_income_jpy;
};

std::span<const OccupationDef> allOccupations();
const OccupationDef* occupationById(std::string_view id);

struct PlaceRef {
  std::string building_id;  // World DB building id (e.g. PLATEAU buildingID); empty = none
  geo::LatLon pos;
  bool valid() const { return !building_id.empty(); }
};

struct Personality {  // Big Five, 0..1
  float openness = 0.5f, conscientiousness = 0.5f, extraversion = 0.5f, agreeableness = 0.5f,
        neuroticism = 0.5f;
};

struct Needs {  // 1 = fully satisfied, 0 = critical
  float hunger = 0.8f, thirst = 0.8f, energy = 0.8f, social = 0.7f, fun = 0.7f, hygiene = 0.9f;
};

struct Health {
  float physical = 1.0f;  // 0..1
  bool sick = false;
  float sickness_severity = 0.0f;  // 0..1
};

struct Emotion {
  float valence = 0.0f;  // -1..1
  float arousal = 0.3f;  // 0..1
};

struct Npc {
  EntityId id = 0;
  std::string family_name, given_name;
  int age = 30;
  Personality personality;
  int wake_minute = 7 * 60;  // chronotype: habitual wake time on workdays
  int sleep_need_min = 7 * 60;
  PlaceRef home, work, school;
  std::string occupation_id;
  int64_t monthly_income_jpy = 0;
  uint64_t bank_account = 0;  // econ::AccountId
  std::optional<EntityId> vehicle;
  std::vector<Hobby> hobbies;
  std::string favorite_music_genre;
  std::string favorite_food;
  Needs needs;
  Health health;
  Emotion emotion;
  std::vector<EntityId> family;
  std::vector<EntityId> friends;
  uint64_t seed = 0;

  std::string fullName() const { return family_name + " " + given_name; }
};

// Procedural resident generator. Homes/workplaces are chosen by the caller
// from real buildings (by usage) in the World Database.
struct ResidentSpec {
  EntityId id;
  uint64_t world_seed;
  PlaceRef home;
  PlaceRef work;  // or school for students
  std::string occupation_id;
};
Npc generateResident(const ResidentSpec& spec);

}  // namespace rj::sim
