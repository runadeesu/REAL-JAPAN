#include <algorithm>

#include "rj/interact/components.hpp"
#include "rj_test.hpp"

using namespace rj::interact;
using rj::econ::AccountKind;
using rj::econ::Ledger;

namespace {
bool offers(const std::vector<VerbOption>& o, Verb v, bool ok) {
  return std::any_of(o.begin(), o.end(), [&](const VerbOption& x) { return x.verb == v && x.check.ok == ok; });
}

struct StoreFixture {
  Ledger ledger;
  World world{&ledger};
  rj::econ::AccountId store = ledger.open(AccountKind::Business, "store");
  Actor player;
  EntityId onigiri = 0, tea = 0, reg = 0, door_out = 0, door_in = 0;

  StoreFixture() {
    player.id = 1;
    player.account = ledger.open(AccountKind::Person, "player");
    ledger.endow(player.account, 1000, 0);
    player.location = "street";
    player.pos = {0, 0, 0};

    Entity& din = world.spawn("door", "street", {1, 0, 0});
    din.add<Portal>().to_location = "store";
    door_in = din.id;

    Entity& o = world.spawn("item.onigiri_salmon", "store", {3, 0, 0});
    o.add<Pickable>();
    auto& m = o.add<Merchandise>();
    m.price = 160;
    m.seller = store;
    m.sku = "onigiri_salmon";
    o.add<Consumable>().satiety = 0.3f;
    onigiri = o.id;

    Entity& t = world.spawn("item.green_tea", "store", {3, 0.5f, 0});
    t.add<Pickable>();
    auto& mt = t.add<Merchandise>();
    mt.price = 150;
    mt.seller = store;
    mt.sku = "green_tea";
    auto& ct = t.add<Consumable>();
    ct.is_drink = true;
    ct.hydration = 0.4f;
    tea = t.id;

    Entity& r = world.spawn("register", "store", {4, 0, 0});
    r.add<Checkout>().business = store;
    reg = r.id;

    Entity& dout = world.spawn("door", "store", {0.5f, 0, 0});
    auto& p = dout.add<Portal>();
    p.to_location = "street";
    p.exits_store_of = store;
    door_out = dout.id;
  }
};
}  // namespace

RJ_TEST(convenience_store_full_flow) {
  StoreFixture f;
  // Enter the store.
  RJ_CHECK(f.world.interact(f.player, f.door_in, Verb::Enter, 10).ok);
  RJ_CHECK_EQ(f.player.location, std::string("store"));
  f.player.pos = {3, 0.2f, 0};
  // Look at the shelf: onigiri can be picked up but not eaten yet.
  auto opts = f.world.options(f.player, f.onigiri, 11);
  RJ_CHECK(offers(opts, Verb::PickUp, true));
  RJ_CHECK(f.world.interact(f.player, f.onigiri, Verb::PickUp, 12).ok);
  RJ_CHECK(f.world.interact(f.player, f.tea, Verb::PickUp, 13).ok);
  // Eating before paying is refused with a localisable reason.
  const Check early = f.world.interact(f.player, f.onigiri, Verb::Eat, 14);
  RJ_CHECK(!early.ok);
  RJ_CHECK_EQ(early.reason, std::string("uis.reason.not_paid"));
  // Hands are full now.
  Entity& extra = f.world.spawn("item.snack", "store", {3, 0.1f, 0});
  extra.add<Pickable>();
  RJ_CHECK_EQ(f.world.interact(f.player, extra.id, Verb::PickUp, 15).reason, std::string("uis.reason.hands_full"));
  // Go to the register and pay.
  f.player.pos = {4, 0.5f, 0};
  RJ_CHECK(f.world.interact(f.player, f.reg, Verb::Buy, 16).ok);
  RJ_CHECK_EQ(f.ledger.balance(f.player.account), 1000 - 310);
  RJ_CHECK_EQ(f.ledger.balance(f.store), 310);
  RJ_CHECK_EQ(f.ledger.sumAll(), 0);
  // Nothing left to pay.
  RJ_CHECK_EQ(f.world.interact(f.player, f.reg, Verb::Buy, 17).reason, std::string("uis.reason.nothing_to_pay"));
  // Eat and drink.
  const float before = f.player.satiety;
  RJ_CHECK(f.world.interact(f.player, f.onigiri, Verb::Eat, 18).ok);
  RJ_CHECK(f.player.satiety > before);
  RJ_CHECK(f.world.find(f.onigiri) == nullptr);
  RJ_CHECK(f.world.interact(f.player, f.tea, Verb::Drink, 19).ok);
  RJ_CHECK(f.player.held.empty());
  // Leave: no shoplifting event.
  f.player.pos = {0.5f, 0, 0};
  RJ_CHECK(f.world.interact(f.player, f.door_out, Verb::Enter, 20).ok);
  for (const auto& e : f.world.events()) RJ_CHECK(e.type != "shoplifting");
}

RJ_TEST(leaving_with_unpaid_goods_raises_event) {
  StoreFixture f;
  f.world.interact(f.player, f.door_in, Verb::Enter, 1);
  f.player.pos = {3, 0, 0};
  f.world.interact(f.player, f.onigiri, Verb::PickUp, 2);
  f.player.pos = {0.5f, 0, 0};
  RJ_CHECK(f.world.interact(f.player, f.door_out, Verb::Enter, 3).ok);
  bool flagged = false;
  for (const auto& e : f.world.events()) flagged |= (e.type == "shoplifting" && e.detail == "onigiri_salmon");
  RJ_CHECK(flagged);
}

RJ_TEST(insufficient_funds_and_reach) {
  StoreFixture f;
  f.world.interact(f.player, f.door_in, Verb::Enter, 1);
  f.player.pos = {3, 0, 0};
  f.ledger.transfer(f.player.account, f.store, 950, rj::econ::TxCategory::Purchase, 1);
  f.world.interact(f.player, f.onigiri, Verb::PickUp, 2);
  f.player.pos = {4, 0, 0};
  RJ_CHECK_EQ(f.world.interact(f.player, f.reg, Verb::Buy, 3).reason, std::string("uis.reason.insufficient_funds"));
  f.player.pos = {40, 0, 0};
  RJ_CHECK_EQ(f.world.interact(f.player, f.reg, Verb::Buy, 4).reason, std::string("uis.reason.too_far"));
}

RJ_TEST(generic_components_sit_drive_repair_work) {
  Ledger ledger;
  World w(&ledger);
  Actor a;
  a.id = 5;
  a.location = "park";
  Entity& bench = w.spawn("bench", "park", {0, 0, 0});
  bench.add<Seat>();
  RJ_CHECK(w.interact(a, bench.id, Verb::Sit, 1).ok);
  RJ_CHECK(a.seated_on.has_value());
  RJ_CHECK(!w.interact(a, bench.id, Verb::Sit, 2).ok);
  RJ_CHECK(w.interact(a, bench.id, Verb::StandUp, 3).ok);

  Entity& car = w.spawn("car.kei", "park", {1, 0, 0});
  car.add<Vehicle>();
  RJ_CHECK_EQ(w.interact(a, car.id, Verb::Drive, 4).reason, std::string("uis.reason.no_license"));
  a.licenses.push_back("driver_license");
  RJ_CHECK(w.interact(a, car.id, Verb::Drive, 5).ok);
  a.vehicle.reset();

  Entity& bike = w.spawn("bicycle", "park", {1, 1, 0});
  auto& cond = bike.add<Condition>();
  RJ_CHECK(w.interact(a, bike.id, Verb::Break, 6).ok);
  RJ_CHECK_EQ(w.interact(a, bike.id, Verb::Repair, 7).reason, std::string("uis.reason.need_tool"));
  Entity& kit = w.spawn("repair_kit", "park", {0, 1, 0});
  kit.add<Pickable>();
  kit.add<Tool>();
  RJ_CHECK(w.interact(a, kit.id, Verb::PickUp, 8).ok);
  RJ_CHECK(w.interact(a, bike.id, Verb::Repair, 9).ok);
  RJ_CHECK_NEAR(cond.condition, 1.0, 1e-9);

  Entity& desk = w.spawn("register_station", "park", {0, 0.5f, 0});
  desk.add<Workstation>().job_id = "shop_clerk";
  RJ_CHECK_EQ(w.interact(a, desk.id, Verb::Work, 10).reason, std::string("uis.reason.not_employed_here"));
  a.job_id = "shop_clerk";
  RJ_CHECK(w.interact(a, desk.id, Verb::Work, 11).ok);
}
