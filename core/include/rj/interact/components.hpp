#pragma once
// Built-in Interactable Components for the UIS. Each is small and
// composable: a convenience-store onigiri is Pickable + Merchandise +
// Consumable; a register is Checkout; a door is Openable + Portal.

#include <optional>
#include <string>
#include <vector>

#include "rj/interact/uis.hpp"

namespace rj::interact {

struct Openable : Component {
  bool open = false;
  bool locked = false;
  std::string_view type() const override { return "openable"; }
  void verbs(std::vector<Verb>& o) const override { o.insert(o.end(), {Verb::Open, Verb::Close}); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Seat : Component {
  std::optional<EntityId> occupant;
  std::string_view type() const override { return "seat"; }
  void verbs(std::vector<Verb>& o) const override { o.insert(o.end(), {Verb::Sit, Verb::StandUp}); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Bed : Component {
  std::optional<EntityId> occupant;
  std::string_view type() const override { return "bed"; }
  void verbs(std::vector<Verb>& o) const override { o.insert(o.end(), {Verb::LieDown, Verb::StandUp}); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Pickable : Component {
  float mass_kg = 0.2f;
  bool two_handed = false;
  std::optional<EntityId> holder;
  std::string_view type() const override { return "pickable"; }
  void verbs(std::vector<Verb>& o) const override {
    o.insert(o.end(), {two_handed ? Verb::Carry : Verb::PickUp, Verb::Drop});
  }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

// Goods for sale. Ownership stays with the seller until paid at a Checkout.
struct Merchandise : Component {
  econ::Yen price = 0;
  econ::AccountId seller = 0;
  bool paid = false;
  std::string sku;
  std::string_view type() const override { return "merchandise"; }
  void verbs(std::vector<Verb>&) const override {}
  Check check(Verb, const Ctx&) const override { return Check::yes(); }
  void apply(Verb, Ctx&) override {}
};

struct Checkout : Component {
  econ::AccountId business = 0;
  bool buys_items = false;
  double buyback_rate = 0.3;
  std::string_view type() const override { return "checkout"; }
  void verbs(std::vector<Verb>& o) const override {
    o.push_back(Verb::Buy);
    if (buys_items) o.push_back(Verb::Sell);
  }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Consumable : Component {
  bool is_drink = false;
  float satiety = 0.25f;
  float hydration = 0.05f;
  bool needs_cooking = false;
  std::string_view type() const override { return "consumable"; }
  void verbs(std::vector<Verb>& o) const override { o.push_back(is_drink ? Verb::Drink : Verb::Eat); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Cookware : Component {
  std::string_view type() const override { return "cookware"; }
  void verbs(std::vector<Verb>& o) const override { o.push_back(Verb::Cook); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Vehicle : Component {
  int seats = 4;
  bool requires_license = true;
  std::optional<EntityId> driver;
  std::vector<EntityId> passengers;
  std::string_view type() const override { return "vehicle"; }
  void verbs(std::vector<Verb>& o) const override { o.insert(o.end(), {Verb::Drive, Verb::Ride}); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

// Books, screens, arcade cabinets, instruments ...
struct Media : Component {
  enum class Kind : uint8_t { Readable, Watchable, Playable } kind = Kind::Readable;
  econ::Yen price_per_use = 0;
  econ::AccountId owner = 0;
  std::string_view type() const override { return "media"; }
  void verbs(std::vector<Verb>& o) const override;
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Workstation : Component {
  std::string job_id;
  std::string_view type() const override { return "workstation"; }
  void verbs(std::vector<Verb>& o) const override { o.push_back(Verb::Work); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Talkable : Component {
  EntityId npc = 0;
  std::string_view type() const override { return "talkable"; }
  void verbs(std::vector<Verb>& o) const override { o.push_back(Verb::Talk); }
  Check check(Verb, const Ctx&) const override { return Check::yes(); }
  void apply(Verb v, Ctx& c) override;
};

struct Tool : Component {
  std::string kind = "repair_kit";
  std::string_view type() const override { return "tool"; }
  void verbs(std::vector<Verb>&) const override {}
  Check check(Verb, const Ctx&) const override { return Check::yes(); }
  void apply(Verb, Ctx&) override {}
};

struct Condition : Component {
  float condition = 1.0f;  // 0 = broken
  float dirt = 0.0f;       // 0 = clean
  std::string_view type() const override { return "condition"; }
  void verbs(std::vector<Verb>& o) const override {
    o.insert(o.end(), {Verb::Repair, Verb::Break, Verb::Clean});
  }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

struct Movable : Component {
  float mass_kg = 20.0f;
  std::string_view type() const override { return "movable"; }
  void verbs(std::vector<Verb>& o) const override { o.insert(o.end(), {Verb::Push, Verb::Pull}); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

// Doorway between spaces. If `exits_store_of` is set, leaving with unpaid
// goods from that seller raises a "shoplifting" event (the player is free
// to do it; the world reacts).
struct Portal : Component {
  std::string to_location;
  Vec3 to_pos;
  econ::AccountId exits_store_of = 0;
  std::string_view type() const override { return "portal"; }
  void verbs(std::vector<Verb>& o) const override { o.push_back(Verb::Enter); }
  Check check(Verb v, const Ctx& c) const override;
  void apply(Verb v, Ctx& c) override;
};

}  // namespace rj::interact
