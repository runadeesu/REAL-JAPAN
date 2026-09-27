#pragma once
// Procedural vehicle models (no third-party assets). Bodies are lofted from per-type profiles
// with per-vertex materials: paint (tinted per vehicle), glass (SSR reflections), black trim,
// white licence plates; lamps are separate meshes so they can light up (head / tail / brake).
// Side panels have real wheel-arch openings with dark wheel wells, door seams, handles, grille;
// wheels have tyres with sidewalls and five-spoke rims (they turn and steer).
// Each type also has a first-person cockpit (dashboard, instrument cluster, steering wheel,
// pillars, roof lining, mirrors, seats, bonnet) for the player's car.

#include "game/traffic.hpp"
#include "raylib.h"

namespace rjc {

struct VehicleModel {
  Mesh body{};
  Mesh wheel{};                 // right-side wheel (outer face +x), centred at the origin
  Mesh head_lamps{}, tail_lamps{};
  float wheel_r = 0.31f;
  float wheel_pos[4][2]{};      // (x right, y forward) per wheel, model space
  // first person
  Mesh cockpit{};               // untinted interior
  Mesh bonnet{};                // painted parts seen from the driver's seat (tinted like the body)
  Mesh steering{};              // steering wheel about its own axis (model +y), centred at the origin
  Mesh gauges{};                // dial markings (drawn emissive)
  Mesh needle{};                // pivot at the origin, pointing up (model +z), in the dial plane
  float steer_pos[3]{};         // steering wheel centre (model space)
  float steer_tilt = 0.45f;     // column angle from horizontal (rad)
  float gauge_pos[2][3]{};      // speedometer, tachometer centres (model space; dial faces -y)
  float mirror_glass[3][4][3]{};  // interior, right, left mirror glass: corners (viewer's bottom-left, bottom-right, top-right, top-left)
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
