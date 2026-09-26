#pragma once
// Universal Interaction System (UIS).
//
// Instead of hand-scripting every human action, world objects are composed
// from Interactable Components. Each component offers verbs (Open, Sit,
// PickUp, Buy, Eat, Drive, Work, Talk ...), checks preconditions and applies
// effects. New behaviour = new component or new verb, with no changes to
// the actor/AI side: players and NPC AI both query `options()` and call
// `interact()` through the same API.
//
// Unavailability reasons are localisation keys (e.g. "uis.reason.hands_full")
// so the UI can show them in Japanese or English.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rj/econ/ledger.hpp"

namespace rj::interact {

using EntityId = uint64_t;

enum class Verb : uint8_t {
  Open, Close, Sit, StandUp, LieDown, PickUp, Carry, Drop, Push, Pull, Use, Eat, Drink, Buy, Sell,
  Repair, Break, Clean, Cook, Drive, Ride, Read, Watch, Play, Work, Talk, Enter, kCount
};
std::string_view verbId(Verb v);  // "open", "sit", ... (localisation key suffix)

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
float distance(const Vec3& a, const Vec3& b);

struct Actor {
  EntityId id = 0;
  std::string name;
  econ::AccountId account = 0;
  std::string location;  // space id, e.g. "street:53393596" or "store:<building>"
  Vec3 pos;
  std::vector<EntityId> held;
  int hands = 2;
  float satiety = 0.5f;    // 1 = full
  float hydration = 0.5f;  // 1 = fully hydrated
  float energy = 0.8f;
  std::optional<EntityId> seated_on, lying_on, vehicle;
  std::vector<std::string> licenses;  // e.g. "driver_license"
  std::string job_id;                 // current job, empty = none

  bool holds(EntityId e) const;
  bool hasLicense(std::string_view l) const;
};

struct Check {
  bool ok = true;
  std::string reason;  // localisation key when !ok
  static Check yes() { return {}; }
  static Check no(std::string key) { return {false, std::move(key)}; }
};

struct Event {
  int64_t unix = 0;
  EntityId actor = 0;
  EntityId target = 0;
  Verb verb = Verb::Use;
  std::string type;    // "interaction", "purchase", "shoplifting", "talk", "work" ...
  std::string detail;
};

class World;
struct Entity;

struct Ctx {
  World& world;
  Actor& actor;
  Entity& target;
  int64_t now;
};

class Component {
 public:
  virtual ~Component() = default;
  virtual std::string_view type() const = 0;
  virtual void verbs(std::vector<Verb>& out) const = 0;
  virtual Check check(Verb v, const Ctx& c) const = 0;
  virtual void apply(Verb v, Ctx& c) = 0;
};

struct Entity {
  EntityId id = 0;
  std::string name_key;  // localisation key or literal
  std::string location;
  Vec3 pos;
  bool alive = true;
  std::vector<std::unique_ptr<Component>> comps;

  template <class T>
  T* get() const {
    for (const auto& c : comps)
      if (auto* p = dynamic_cast<T*>(c.get())) return p;
    return nullptr;
  }
  template <class T, class... Args>
  T& add(Args&&... args) {
    comps.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    return static_cast<T&>(*comps.back());
  }
};

struct VerbOption {
  Verb verb;
  Check check;
  std::string_view component;
};

class World {
 public:
  explicit World(econ::Ledger* ledger = nullptr) : ledger_(ledger) {}

  Entity& spawn(std::string name_key, std::string location, Vec3 pos);
  Entity* find(EntityId id);
  void destroy(EntityId id);
  std::vector<EntityId> entitiesAt(std::string_view location) const;

  // Every verb the target offers, with availability for this actor.
  std::vector<VerbOption> options(Actor& actor, EntityId target, int64_t now);
  // Execute `verb` on `target`. Uses the first component whose check passes.
  Check interact(Actor& actor, EntityId target, Verb verb, int64_t now);

  void emit(Event e) { events_.push_back(std::move(e)); }
  const std::vector<Event>& events() const { return events_; }
  econ::Ledger* ledger() { return ledger_; }

  float reach_m = 2.0f;

 private:
  Check inReach(const Actor& a, const Entity& e) const;
  econ::Ledger* ledger_;
  EntityId next_id_ = 1;
  std::unordered_map<EntityId, std::unique_ptr<Entity>> entities_;
  std::vector<Event> events_;
};

}  // namespace rj::interact
