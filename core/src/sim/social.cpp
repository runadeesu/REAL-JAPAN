#include "rj/sim/social.hpp"

#include <algorithm>
#include <cmath>

namespace rj::sim {

std::string_view relationshipName(RelationshipLevel l) {
  switch (l) {
    case RelationshipLevel::Unknown: return "Unknown";
    case RelationshipLevel::Acquaintance: return "Acquaintance";
    case RelationshipLevel::Friend: return "Friend";
    case RelationshipLevel::CloseFriend: return "CloseFriend";
    case RelationshipLevel::Partner: return "Partner";
    case RelationshipLevel::Family: return "Family";
    case RelationshipLevel::Enemy: return "Enemy";
  }
  return "?";
}

void SocialMemory::recordEncounter(EntityId other, int64_t unix, float affinity_delta,
                                   std::string place_id, std::string summary, float salience) {
  decay(unix);
  Relationship& r = rel_[other];
  r.familiarity = std::min(1.0f, r.familiarity + p_.familiarity_gain * (1.0f - r.familiarity));
  r.affinity = std::clamp(r.affinity + affinity_delta, -1.0f, 1.0f);
  if (affinity_delta > 0) r.trust = std::min(1.0f, r.trust + 0.5f * affinity_delta);
  if (affinity_delta < 0) r.trust = std::max(0.0f, r.trust + affinity_delta);
  ++r.encounters;
  r.last_seen_unix = unix;

  memories_.push_back({unix, other, std::move(place_id), std::move(summary), salience});
  if (memories_.size() > p_.memory_capacity) {
    // Forget the least salient (older first on ties).
    auto victim = std::min_element(memories_.begin(), memories_.end(), [](const auto& a, const auto& b) {
      if (a.salience != b.salience) return a.salience < b.salience;
      return a.unix < b.unix;
    });
    memories_.erase(victim);
  }
}

void SocialMemory::setBond(EntityId other, RelationshipLevel bond) {
  if (bond == RelationshipLevel::Partner || bond == RelationshipLevel::Family) rel_[other].bond = bond;
}

void SocialMemory::decay(int64_t now) {
  if (now <= last_decay_unix_) return;
  for (auto& [id, r] : rel_) {
    if (r.last_seen_unix == 0) continue;
    const int64_t from = std::max(r.last_seen_unix, last_decay_unix_);
    if (now <= from) continue;
    const double days = static_cast<double>(now - from) / 86400.0;
    r.familiarity *= static_cast<float>(std::pow(0.5, days / p_.familiarity_half_life_days));
  }
  last_decay_unix_ = now;
}

const Relationship* SocialMemory::get(EntityId other) const {
  auto it = rel_.find(other);
  return it == rel_.end() ? nullptr : &it->second;
}

RelationshipLevel SocialMemory::level(EntityId other) const {
  const Relationship* r = get(other);
  if (!r) return RelationshipLevel::Unknown;
  if (r->bond == RelationshipLevel::Partner || r->bond == RelationshipLevel::Family) return r->bond;
  if (r->affinity <= -0.6f) return RelationshipLevel::Enemy;
  if (r->familiarity < p_.recognition_threshold) return RelationshipLevel::Unknown;
  if (r->familiarity >= 0.7f && r->affinity >= 0.6f && r->trust >= 0.6f) return RelationshipLevel::CloseFriend;
  if (r->familiarity >= 0.4f && r->affinity >= 0.3f) return RelationshipLevel::Friend;
  return RelationshipLevel::Acquaintance;
}

bool SocialMemory::recognizes(EntityId other) const {
  const Relationship* r = get(other);
  return r && r->familiarity >= p_.recognition_threshold;
}

std::vector<EpisodicMemory> SocialMemory::recall(EntityId other, size_t k) const {
  std::vector<EpisodicMemory> out;
  for (const auto& m : memories_)
    if (m.about == other) out.push_back(m);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.salience != b.salience) return a.salience > b.salience;
    return a.unix > b.unix;
  });
  if (out.size() > k) out.resize(k);
  return out;
}

}  // namespace rj::sim
