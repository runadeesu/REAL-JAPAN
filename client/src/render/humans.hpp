#pragma once
// Procedural human figures (no third-party assets): a skeleton-driven parametric body built from
// lofted ellipses (torso, limbs), head, hair, shoes; body variants (trousers / skirt, short / long
// hair). A walk cycle is baked into 16 frames per variant (+ an idle pose), so each person is one
// draw call; clothing, skin and hair colours are per-person shader uniforms (material ids 31 top,
// 36 bottom, 32 skin, 37 hair).

#include "raylib.h"

namespace rjc {

enum class BodyVariant : int { Trousers = 0, TrousersLongHair, Skirt, Count };
enum class StillPose : int { Sit = 0, Strap, Count };  // seated (feet on the floor under the origin's front), holding a strap

class HumanModels {
 public:
  static constexpr int kFrames = 16;
  void build();
  void unload();
  bool ready() const { return ready_; }
  // phase in radians (walk cycle); idle = standing still
  const Mesh& frame(BodyVariant v, float phase, bool idle) const;
  const Mesh& still(BodyVariant v, StillPose p) const { return still_[static_cast<int>(v)][static_cast<int>(p)]; }
  // Umbrella held in the right hand above the head (canopy material 31 = tinted per draw, shaft 29).
  const Mesh& umbrella() const { return umbrella_; }

 private:
  Mesh walk_[static_cast<int>(BodyVariant::Count)][kFrames]{};
  Mesh idle_[static_cast<int>(BodyVariant::Count)]{};
  Mesh still_[static_cast<int>(BodyVariant::Count)][static_cast<int>(StillPose::Count)]{};
  Mesh umbrella_{};
  bool ready_ = false;
};

}  // namespace rjc
