#include "rj/sim/social.hpp"
#include "rj_test.hpp"

using namespace rj::sim;

RJ_TEST(clerk_recognises_regular_customer) {
  SocialMemory clerk;
  const EntityId player = 1;
  const int64_t day = 86400;
  const int64_t t0 = 1790000000;
  clerk.recordEncounter(player, t0, 0.02f, "konbini", "おにぎりを買った");
  RJ_CHECK(!clerk.recognizes(player));
  RJ_CHECK(clerk.level(player) == RelationshipLevel::Unknown);
  clerk.recordEncounter(player, t0 + 2 * day, 0.02f, "konbini", "お茶を買った");
  clerk.recordEncounter(player, t0 + 4 * day, 0.02f, "konbini", "弁当を買った");
  RJ_CHECK(clerk.recognizes(player));
  RJ_CHECK(clerk.level(player) == RelationshipLevel::Acquaintance);
  const auto mem = clerk.recall(player, 2);
  RJ_CHECK_EQ(mem.size(), 2u);
  RJ_CHECK_EQ(mem[0].summary, std::string("弁当を買った"));  // most recent among equal salience
  // Half a year without visiting: familiarity decays and the clerk forgets the face.
  clerk.decay(t0 + 184 * day);
  RJ_CHECK(!clerk.recognizes(player));
}

RJ_TEST(friendship_enemy_and_bonds) {
  SocialMemory m;
  const int64_t t = 1790000000;
  for (int i = 0; i < 12; ++i) m.recordEncounter(2, t + i * 3600, 0.08f, "cafe", "話した");
  RJ_CHECK(m.level(2) == RelationshipLevel::CloseFriend || m.level(2) == RelationshipLevel::Friend);
  for (int i = 0; i < 5; ++i) m.recordEncounter(3, t + i * 3600, -0.3f, "street", "口論した", 0.9f);
  RJ_CHECK(m.level(3) == RelationshipLevel::Enemy);
  m.setBond(4, RelationshipLevel::Family);
  RJ_CHECK(m.level(4) == RelationshipLevel::Family);
  m.setBond(5, RelationshipLevel::Friend);  // not a bond type: ignored
  RJ_CHECK(m.level(5) == RelationshipLevel::Unknown);
}

RJ_TEST(memory_capacity_forgets_least_salient) {
  SocialParams p;
  p.memory_capacity = 3;
  SocialMemory m(p);
  m.recordEncounter(9, 100, 0, "a", "important", 0.9f);
  m.recordEncounter(9, 200, 0, "b", "trivial", 0.1f);
  m.recordEncounter(9, 300, 0, "c", "normal", 0.5f);
  m.recordEncounter(9, 400, 0, "d", "normal2", 0.5f);
  RJ_CHECK_EQ(m.memoryCount(), 3u);
  bool has_trivial = false;
  for (const auto& e : m.recall(9, 10)) has_trivial |= e.summary == "trivial";
  RJ_CHECK(!has_trivial);
}
