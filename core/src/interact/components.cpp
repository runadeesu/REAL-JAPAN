#include "rj/interact/components.hpp"

#include <algorithm>
#include <cmath>

namespace rj::interact {
namespace {

int handsUsed(World& w, const Actor& a) {
  int used = 0;
  for (EntityId id : a.held) {
    Entity* e = w.find(id);
    const Pickable* p = e ? e->get<Pickable>() : nullptr;
    used += (p && p->two_handed) ? 2 : 1;
  }
  return used;
}

void release(Actor& a, EntityId id) { a.held.erase(std::remove(a.held.begin(), a.held.end(), id), a.held.end()); }

bool unpaid(const Entity& e) {
  const Merchandise* m = e.get<Merchandise>();
  return m && !m->paid;
}

}  // namespace

// --- Openable ---------------------------------------------------------------
Check Openable::check(Verb v, const Ctx&) const {
  if (v == Verb::Open) {
    if (locked) return Check::no("uis.reason.locked");
    if (open) return Check::no("uis.reason.already_open");
  } else if (!open) {
    return Check::no("uis.reason.already_closed");
  }
  return Check::yes();
}
void Openable::apply(Verb v, Ctx&) { open = (v == Verb::Open); }

// --- Seat / Bed -------------------------------------------------------------
Check Seat::check(Verb v, const Ctx& c) const {
  if (v == Verb::Sit) {
    if (occupant) return Check::no("uis.reason.occupied");
    if (c.actor.seated_on || c.actor.lying_on) return Check::no("uis.reason.already_seated");
  } else if (occupant != c.actor.id) {
    return Check::no("uis.reason.not_seated_here");
  }
  return Check::yes();
}
void Seat::apply(Verb v, Ctx& c) {
  if (v == Verb::Sit) {
    occupant = c.actor.id;
    c.actor.seated_on = c.target.id;
  } else {
    occupant.reset();
    c.actor.seated_on.reset();
  }
}

Check Bed::check(Verb v, const Ctx& c) const {
  if (v == Verb::LieDown) {
    if (occupant) return Check::no("uis.reason.occupied");
    if (c.actor.seated_on || c.actor.lying_on) return Check::no("uis.reason.already_seated");
  } else if (occupant != c.actor.id) {
    return Check::no("uis.reason.not_seated_here");
  }
  return Check::yes();
}
void Bed::apply(Verb v, Ctx& c) {
  if (v == Verb::LieDown) {
    occupant = c.actor.id;
    c.actor.lying_on = c.target.id;
  } else {
    occupant.reset();
    c.actor.lying_on.reset();
  }
}

// --- Pickable ---------------------------------------------------------------
Check Pickable::check(Verb v, const Ctx& c) const {
  if (v == Verb::Drop) return c.actor.holds(c.target.id) ? Check::yes() : Check::no("uis.reason.not_holding");
  if (holder) return Check::no("uis.reason.held_by_someone");
  if (mass_kg > 40.0f) return Check::no("uis.reason.too_heavy");
  const int need = two_handed ? 2 : 1;
  if (handsUsed(c.world, c.actor) + need > c.actor.hands) return Check::no("uis.reason.hands_full");
  return Check::yes();
}
void Pickable::apply(Verb v, Ctx& c) {
  if (v == Verb::Drop) {
    release(c.actor, c.target.id);
    holder.reset();
    c.target.location = c.actor.location;
    c.target.pos = c.actor.pos;
  } else {
    holder = c.actor.id;
    c.actor.held.push_back(c.target.id);
  }
}

// --- Checkout (Buy / Sell) -------------------------------------------------
Check Checkout::check(Verb v, const Ctx& c) const {
  if (!c.world.ledger()) return Check::no("uis.reason.no_economy");
  if (v == Verb::Buy) {
    econ::Yen total = 0;
    for (EntityId id : c.actor.held) {
      Entity* e = c.world.find(id);
      const Merchandise* m = e ? e->get<Merchandise>() : nullptr;
      if (m && !m->paid && m->seller == business) total += m->price;
    }
    if (total == 0) return Check::no("uis.reason.nothing_to_pay");
    const econ::Account* acc = c.world.ledger()->account(c.actor.account);
    if (!acc || acc->balance + acc->credit_limit < total) return Check::no("uis.reason.insufficient_funds");
    return Check::yes();
  }
  // Sell: the first held, paid item.
  for (EntityId id : c.actor.held) {
    Entity* e = c.world.find(id);
    const Merchandise* m = e ? e->get<Merchandise>() : nullptr;
    if (m && m->paid) {
      const auto price = static_cast<econ::Yen>(std::llround(static_cast<double>(m->price) * buyback_rate));
      if (c.world.ledger()->balance(business) < price) return Check::no("uis.reason.store_cannot_pay");
      return Check::yes();
    }
  }
  return Check::no("uis.reason.nothing_to_sell");
}

void Checkout::apply(Verb v, Ctx& c) {
  econ::Ledger& L = *c.world.ledger();
  if (v == Verb::Buy) {
    econ::Yen total = 0;
    std::vector<Merchandise*> items;
    for (EntityId id : c.actor.held) {
      Entity* e = c.world.find(id);
      Merchandise* m = e ? e->get<Merchandise>() : nullptr;
      if (m && !m->paid && m->seller == business) {
        total += m->price;
        items.push_back(m);
      }
    }
    if (L.transfer(c.actor.account, business, total, econ::TxCategory::Purchase, c.now, "checkout") ==
        econ::TxResult::Ok) {
      for (Merchandise* m : items) m->paid = true;
      c.world.emit({c.now, c.actor.id, c.target.id, v, "purchase", std::to_string(total)});
    }
    return;
  }
  for (EntityId id : c.actor.held) {
    Entity* e = c.world.find(id);
    Merchandise* m = e ? e->get<Merchandise>() : nullptr;
    if (m && m->paid) {
      const auto price = static_cast<econ::Yen>(std::llround(static_cast<double>(m->price) * buyback_rate));
      if (L.transfer(business, c.actor.account, price, econ::TxCategory::Purchase, c.now, "buyback") ==
          econ::TxResult::Ok) {
        m->paid = false;
        m->seller = business;
        release(c.actor, id);
        if (Pickable* p = e->get<Pickable>()) p->holder.reset();
        e->location = c.target.location;
        e->pos = c.target.pos;
        c.world.emit({c.now, c.actor.id, c.target.id, v, "sale", std::to_string(price)});
      }
      return;
    }
  }
}

// --- Consumable / Cookware -------------------------------------------------
Check Consumable::check(Verb, const Ctx& c) const {
  if (!c.actor.holds(c.target.id)) return Check::no("uis.reason.not_holding");
  if (unpaid(c.target)) return Check::no("uis.reason.not_paid");
  if (needs_cooking) return Check::no("uis.reason.needs_cooking");
  return Check::yes();
}
void Consumable::apply(Verb, Ctx& c) {
  c.actor.satiety = std::min(1.0f, c.actor.satiety + satiety);
  c.actor.hydration = std::min(1.0f, c.actor.hydration + hydration);
  release(c.actor, c.target.id);
  c.world.destroy(c.target.id);
}

Check Cookware::check(Verb, const Ctx& c) const {
  for (EntityId id : c.actor.held) {
    Entity* e = c.world.find(id);
    const Consumable* f = e ? e->get<Consumable>() : nullptr;
    if (f && f->needs_cooking) return unpaid(*e) ? Check::no("uis.reason.not_paid") : Check::yes();
  }
  return Check::no("uis.reason.no_ingredient");
}
void Cookware::apply(Verb, Ctx& c) {
  for (EntityId id : c.actor.held) {
    Entity* e = c.world.find(id);
    Consumable* f = e ? e->get<Consumable>() : nullptr;
    if (f && f->needs_cooking) {
      f->needs_cooking = false;
      f->satiety *= 1.5f;
      return;
    }
  }
}

// --- Vehicle ---------------------------------------------------------------
Check Vehicle::check(Verb v, const Ctx& c) const {
  if (c.actor.vehicle) return Check::no("uis.reason.already_in_vehicle");
  if (v == Verb::Drive) {
    if (driver) return Check::no("uis.reason.occupied");
    if (requires_license && !c.actor.hasLicense("driver_license")) return Check::no("uis.reason.no_license");
    return Check::yes();
  }
  if (static_cast<int>(passengers.size()) + 1 >= seats) return Check::no("uis.reason.vehicle_full");
  return Check::yes();
}
void Vehicle::apply(Verb v, Ctx& c) {
  if (v == Verb::Drive) driver = c.actor.id;
  else passengers.push_back(c.actor.id);
  c.actor.vehicle = c.target.id;
}

// --- Media -------------------------------------------------------------------
void Media::verbs(std::vector<Verb>& o) const {
  switch (kind) {
    case Kind::Readable: o.push_back(Verb::Read); break;
    case Kind::Watchable: o.push_back(Verb::Watch); break;
    case Kind::Playable: o.push_back(Verb::Play); break;
  }
}
Check Media::check(Verb, const Ctx& c) const {
  if (price_per_use > 0) {
    if (!c.world.ledger()) return Check::no("uis.reason.no_economy");
    const econ::Account* a = c.world.ledger()->account(c.actor.account);
    if (!a || a->balance + a->credit_limit < price_per_use) return Check::no("uis.reason.insufficient_funds");
  }
  return Check::yes();
}
void Media::apply(Verb v, Ctx& c) {
  if (price_per_use > 0)
    c.world.ledger()->transfer(c.actor.account, owner, price_per_use, econ::TxCategory::Purchase, c.now,
                               std::string(verbId(v)));
}

// --- Workstation / Talkable ---------------------------------------------------
Check Workstation::check(Verb, const Ctx& c) const {
  return c.actor.job_id == job_id ? Check::yes() : Check::no("uis.reason.not_employed_here");
}
void Workstation::apply(Verb v, Ctx& c) { c.world.emit({c.now, c.actor.id, c.target.id, v, "work", job_id}); }

void Talkable::apply(Verb v, Ctx& c) {
  c.world.emit({c.now, c.actor.id, c.target.id, v, "talk", std::to_string(npc)});
}

// --- Condition (Repair / Break / Clean) --------------------------------------
Check Condition::check(Verb v, const Ctx& c) const {
  switch (v) {
    case Verb::Break:
      return condition > 0.0f ? Check::yes() : Check::no("uis.reason.already_broken");
    case Verb::Clean:
      return dirt > 0.0f ? Check::yes() : Check::no("uis.reason.already_clean");
    case Verb::Repair: {
      if (condition >= 1.0f) return Check::no("uis.reason.not_damaged");
      for (EntityId id : c.actor.held) {
        Entity* e = c.world.find(id);
        if (e && e->get<Tool>()) return Check::yes();
      }
      return Check::no("uis.reason.need_tool");
    }
    default:
      return Check::no("uis.reason.verb_not_offered");
  }
}
void Condition::apply(Verb v, Ctx& c) {
  if (v == Verb::Break) {
    condition = 0.0f;
    c.world.emit({c.now, c.actor.id, c.target.id, v, "damage", ""});
  } else if (v == Verb::Clean) {
    dirt = 0.0f;
  } else if (v == Verb::Repair) {
    condition = 1.0f;
  }
}

// --- Movable -------------------------------------------------------------------
Check Movable::check(Verb, const Ctx& c) const {
  if (mass_kg > 250.0f) return Check::no("uis.reason.too_heavy");
  if (c.actor.seated_on || c.actor.lying_on) return Check::no("uis.reason.already_seated");
  return Check::yes();
}
void Movable::apply(Verb v, Ctx& c) {
  Vec3 d{c.target.pos.x - c.actor.pos.x, c.target.pos.y - c.actor.pos.y, 0.0f};
  const float len = std::sqrt(d.x * d.x + d.y * d.y);
  if (len < 1e-4f) return;
  const float step = (v == Verb::Push ? 0.5f : -0.5f) / len;
  c.target.pos.x += d.x * step;
  c.target.pos.y += d.y * step;
}

// --- Portal ---------------------------------------------------------------------
Check Portal::check(Verb, const Ctx& c) const {
  if (const Openable* o = c.target.get<Openable>(); o && !o->open) return Check::no("uis.reason.closed");
  if (c.actor.seated_on || c.actor.lying_on || c.actor.vehicle) return Check::no("uis.reason.already_seated");
  return Check::yes();
}
void Portal::apply(Verb v, Ctx& c) {
  if (exits_store_of != 0) {
    for (EntityId id : c.actor.held) {
      Entity* e = c.world.find(id);
      const Merchandise* m = e ? e->get<Merchandise>() : nullptr;
      if (m && !m->paid && m->seller == exits_store_of)
        c.world.emit({c.now, c.actor.id, id, v, "shoplifting", m->sku});
    }
  }
  c.actor.location = to_location;
  c.actor.pos = to_pos;
  for (EntityId id : c.actor.held) {
    if (Entity* e = c.world.find(id)) {
      e->location = to_location;
      e->pos = to_pos;
    }
  }
}

}  // namespace rj::interact
