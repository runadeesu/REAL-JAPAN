#pragma once
// Player controller: walking on the real terrain with building collision,
// running, jumping, and a free-fly exploration mode. First/third person.

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;
class Interior;
struct Settings;

struct Player {
  rj::geo::Vec3d pos;  // feet, origin ENU (metres)
  double vel_z = 0.0;
  float yaw = 0.0f;    // radians, 0 = north, clockwise positive (compass heading)
  float pitch = 0.0f;  // radians, + looks up
  bool fly = false;
  bool grounded = false;
  int camera_mode = 0;  // 0 = first person, 1 = third person
  double distance_walked = 0.0;
  float auto_forward_s = 0.0f;  // scripted forward walking (tests / demos)
  bool left_interior = false;   // set by update(): walked out of a stairwell onto the pavement

  static constexpr float kEyeHeight = 1.60f;
  static constexpr float kRadius = 0.35f;

  // inside: the verified interior the player is in; nearby (when outside): an interior whose
  // stairwell parapets should block. Sets left_interior when the player stepped out onto the street.
  void update(float dt, const World& world, const Settings& s, bool input_enabled, const Interior* inside = nullptr,
              const Interior* nearby = nullptr);
  void snapToGround(const World& world);
  Camera3D camera(float fov_deg, float third_person_dist = 4.5f) const;
  rj::geo::Vec3d eyeEnu() const { return {pos.x, pos.y, pos.z + kEyeHeight}; }
  rj::geo::Vec3d forwardEnu() const;
};

}  // namespace rjc
