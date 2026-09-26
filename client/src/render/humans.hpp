#pragma once
// Procedural human figures (no third-party assets): a skeleton-driven parametric body built from
// lofted ellipses (torso, limbs), head, hair, shoes; body variants (trousers / skirt, short / long
// hair). A walk cycle is baked into 16 frames per variant (+ an idle pose), so each person is one
// draw call; clothing, skin and hair colours are per-person shader uniforms (material ids 31 top,
// 36 bottom, 32 skin, 37 hair).

#include "raylib.h"

namespace rjc {

enum class BodyVariant : int { Trousers = 0, TrousersLongHair, Skirt, Count };

class HumanModels {
 public:
  static constexpr int kFrames = 16;
  void build();
  void unload();
  bool ready() const { return ready_; }
  // phase in radians (walk cycle); idle = standing still
  const Mesh& frame(BodyVariant v, float phase, bool idle) const;

 private:
  Mesh walk_[static_cast<int>(BodyVariant::Count)][kFrames]{};
  Mesh idle_[static_cast<int>(BodyVariant::Count)]{};
  bool ready_ = false;
};

}  // namespace rjc
