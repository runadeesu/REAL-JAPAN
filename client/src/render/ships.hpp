#pragma once
// Procedural ferries (no third-party assets; no real vessel is reproduced): lofted hull with a
// raked bow, antifouling below the waterline, boot-top and a livery stripe; deckhouses with
// window bands, the open passenger deck with railings, bridge with wings, funnel, mast and radar,
// life-raft canisters. Model space: x right, y forward, z up from the waterline.

#include "raylib.h"

namespace rjc {

struct ShipModel {
  Mesh hull{};
  float length = 60.0f;
};

class ShipModels {
 public:
  void build();
  void unload();
  bool ready() const { return ready_; }
  const ShipModel& get(int cls) const { return m_[cls < 0 ? 0 : cls > 1 ? 1 : cls]; }

 private:
  ShipModel m_[2];
  bool ready_ = false;
};

}  // namespace rjc
