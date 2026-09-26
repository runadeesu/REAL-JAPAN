#include "rj/interact/uis.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace rj::interact {

std::string_view verbId(Verb v) {
  static constexpr std::array<std::string_view, static_cast<size_t>(Verb::kCount)> kIds{
      "open", "close", "sit", "stand_up", "lie_down", "pick_up", "carry", "drop", "push", "pull",
      "use", "eat", "drink", "buy", "sell", "repair", "break", "clean", "cook", "drive", "ride",
      "read", "watch", "play", "work", "talk", "enter"};
  const auto i = static_cast<size_t>(v);
  return i < kIds.size() ? kIds[i] : "?";
}

float distance(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Actor::holds(EntityId e) const { return std::find(held.begin(), held.end(), e) != held.end(); }

bool Actor::hasLicense(std::string_view l) const {
  return std::find(licenses.begin(), licenses.end(), l) != licenses.end();
}

Entity& World::spawn(std::string name_key, std::string location, Vec3 pos) {
  auto e = std::make_unique<Entity>();
  e->id = next_id_++;
  e->name_key = std::move(name_key);
  e->location = std::move(location);
  e->pos = pos;
  Entity& ref = *e;
  entities_[ref.id] = std::move(e);
  return ref;
}

Entity* World::find(EntityId id) {
  auto it = entities_.find(id);
  return it == entities_.end() || !it->second->alive ? nullptr : it->second.get();
}

void World::destroy(EntityId id) {
  auto it = entities_.find(id);
  if (it != entities_.end()) it->second->alive = false;
}

std::vector<EntityId> World::entitiesAt(std::string_view location) const {
  std::vector<EntityId> out;
  for (const auto& [id, e] : entities_)
    if (e->alive && e->location == location) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

Check World::inReach(const Actor& a, const Entity& e) const {
  if (a.holds(e.id)) return Check::yes();
  if (a.location != e.location) return Check::no("uis.reason.not_here");
  if (distance(a.pos, e.pos) > reach_m) return Check::no("uis.reason.too_far");
  return Check::yes();
}

std::vector<VerbOption> World::options(Actor& actor, EntityId target, int64_t now) {
  std::vector<VerbOption> out;
  Entity* e = find(target);
  if (!e) return out;
  const Check reach = inReach(actor, *e);
  Ctx ctx{*this, actor, *e, now};
  std::vector<Verb> vs;
  for (const auto& c : e->comps) {
    vs.clear();
    c->verbs(vs);
    for (Verb v : vs) out.push_back({v, reach.ok ? c->check(v, ctx) : reach, c->type()});
  }
  return out;
}

Check World::interact(Actor& actor, EntityId target, Verb verb, int64_t now) {
  Entity* e = find(target);
  if (!e) return Check::no("uis.reason.no_target");
  if (Check r = inReach(actor, *e); !r.ok) return r;
  Ctx ctx{*this, actor, *e, now};
  Check last = Check::no("uis.reason.verb_not_offered");
  std::vector<Verb> vs;
  for (auto& c : e->comps) {
    vs.clear();
    c->verbs(vs);
    if (std::find(vs.begin(), vs.end(), verb) == vs.end()) continue;
    last = c->check(verb, ctx);
    if (!last.ok) continue;
    c->apply(verb, ctx);
    emit({now, actor.id, target, verb, "interaction", std::string(verbId(verb))});
    return last;
  }
  return last;
}

}  // namespace rj::interact
