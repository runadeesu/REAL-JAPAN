#pragma once
// Procedural vehicle models (no third-party assets). Bodies are lofted from per-type profiles
// with per-vertex materials: paint (tinted per vehicle), glass (SSR reflections), black trim,
// white licence plates; lamps are separate meshes so they can light up (head / tail / brake).

#include "game/traffic.hpp"
#include "raylib.h"

namespace rjc {

struct VehicleModel {
  Mesh body{};
  Mesh wheel{};
  Mesh head_lamps{}, tail_lamps{};
  float wheel_r = 0.31f;
  float wheel_pos[4][2]{};  // (x right, y forward) per wheel, model space
};

class VehicleModels {
 public:
  void build();
  void unload();
  const VehicleModel& get(VehicleType t) const { return models_[static_cast<int>(t)]; }
  bool ready() const { return ready_; }

 private:
  VehicleModel models_[static_cast<int>(VehicleType::Count)];
  bool ready_ = false;
};

}  // namespace rjc
