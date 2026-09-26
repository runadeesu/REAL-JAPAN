#pragma once
// NPC memory and relationships. NPCs remember who they met, where and how
// it went; familiarity decays when people stop seeing each other. A shop
// clerk starts recognising a regular after a few visits.
//
// Relationship levels: Unknown, Acquaintance, Friend, CloseFriend,
// Partner, Family, Enemy. Partner/Family are bonds set by life events and
// are not derived from encounter statistics.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rj/sim/npc.hpp"

namespace rj::sim {

enum class RelationshipLevel : uint8_t { Unknown, Acquaintance, Friend, CloseFriend, Partner, Family, Enemy };
std::string_view relationshipName(RelationshipLevel l);

struct Relationship {
  float familiarity = 0.0f;  // 0..1, grows with encounters, decays with absence
  float affinity = 0.0f;     // -1..1, how much they like the other
  float trust = 0.0f;        // 0..1
  int encounters = 0;
  int64_t last_seen_unix = 0;
  RelationshipLevel bond = RelationshipLevel::Unknown;  // Partner/Family when set
};

struct EpisodicMemory {
  int64_t unix = 0;
  EntityId about = 0;
  std::string place_id;
  std::string summary;  // short factual description, e.g. "おにぎりを買った"
  float salience = 0.5f;
};

struct SocialParams {
  float familiarity_gain = 0.12f;       // per encounter, scaled by (1 - familiarity)
  float recognition_threshold = 0.2f;   // familiarity needed to recognise someone
  double familiarity_half_life_days = 45.0;
  size_t memory_capacity = 128;
};

class SocialMemory {
 public:
  explicit SocialMemory(SocialParams p = {}) : p_(p) {}

  void recordEncounter(EntityId other, int64_t unix, float affinity_delta, std::string place_id,
                       std::string summary, float salience = 0.5f);
  void setBond(EntityId other, RelationshipLevel bond);  // Partner or Family
  // Apply time decay of familiarity up to `now`.
  void decay(int64_t now_unix);

  const Relationship* get(EntityId other) const;
  RelationshipLevel level(EntityId other) const;
  bool recognizes(EntityId other) const;
  // Most salient/recent memories about `other`, newest first among equals.
  std::vector<EpisodicMemory> recall(EntityId other, size_t k) const;
  size_t memoryCount() const { return memories_.size(); }

 private:
  SocialParams p_;
  std::unordered_map<EntityId, Relationship> rel_;
  std::vector<EpisodicMemory> memories_;
  int64_t last_decay_unix_ = 0;
};

}  // namespace rj::sim
