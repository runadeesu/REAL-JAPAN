#include "game/player.hpp"

#include <algorithm>
#include <cmath>

#include "config/settings.hpp"
#include "raymath.h"
#include "world/coords.hpp"
#include "world/interior.hpp"
#include "world/world.hpp"

namespace rjc {

rj::geo::Vec3d Player::forwardEnu() const {
  return {std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
}

void Player::snapToGround(const World& world) {
  if (auto h = world.terrainHeight(pos.x, pos.y)) {
    pos.z = *h;
    vel_z = 0;
    grounded = true;
  }
}

void Player::update(float dt, const World& world, const Settings& s, bool input, const Interior* inside,
                    const Interior* nearby) {
  dt = std::min(dt, 0.1f);
  if (input) {
    const Vector2 md = GetMouseDelta();
    const float sens = 0.0022f * s.mouse_sensitivity;
    yaw += md.x * sens;
    pitch += (s.invert_y ? md.y : -md.y) * sens;
    pitch = std::clamp(pitch, -1.45f, 1.45f);
    if (yaw > PI) yaw -= 2 * PI;
    if (yaw < -PI) yaw += 2 * PI;
    if (IsKeyPressed(KEY_F)) {
      fly = !fly;
      vel_z = 0;
    }
    if (IsKeyPressed(KEY_V)) camera_mode = 1 - camera_mode;
  }

  double fx = 0, fy = 0;
  if (input) {
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) { fx += sy; fy += cy; }
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) { fx -= sy; fy -= cy; }
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) { fx += cy; fy -= sy; }
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) { fx -= cy; fy += sy; }
  }
  if (auto_forward_s > 0.0f) {
    auto_forward_s -= dt;
    fx += std::sin(yaw);
    fy += std::cos(yaw);
  }
  const double len = std::hypot(fx, fy);
  if (len > 1e-6) {
    fx /= len;
    fy /= len;
  }
  const bool run = input && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
  const rj::geo::Vec3d before = pos;

  if (fly) {
    const double speed = run ? 60.0 : 15.0;
    pos.x += fx * speed * dt;
    pos.y += fy * speed * dt;
    if (input && IsKeyDown(KEY_SPACE)) pos.z += speed * dt;
    if (input && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_C))) pos.z -= speed * dt;
    if (!inside)
      if (auto h = world.terrainHeight(pos.x, pos.y)) pos.z = std::max(pos.z, *h);
    grounded = false;
  } else {
    const double speed = run ? 4.2 : 1.45;  // m/s: brisk run / normal walking pace
    pos.x += fx * speed * dt;
    pos.y += fy * speed * dt;
    std::optional<double> ground;
    left_interior = false;
    if (inside) {
      // Verified interior: walls block, floors and stair ramps carry the player (step up to 0.55 m).
      // Walking never drops more than 2.5 m (no stepping off through a slab); falling can go further.
      inside->collide(pos, 0.3);
      ground = inside->floorBelow(pos.x, pos.y, pos.z + 0.55, grounded ? 2.5 : 12.0);
      if (!ground) {
        // Top of a stairwell: the next step is the pavement.
        if (auto th = world.terrainHeight(pos.x, pos.y); th && *th <= pos.z + 0.55 && *th >= pos.z - 1.0) {
          ground = th;
          left_interior = true;
        }
      }
    } else {
      world.collide(pos, kRadius);
      if (nearby) nearby->collide(pos, kRadius);  // stairwell parapets that stand above the pavement
      ground = world.terrainHeight(pos.x, pos.y);
    }
    if (!ground) {
      // Outside the loaded data: stay on the edge.
      pos.x = before.x;
      pos.y = before.y;
    }
    const double gz = ground.value_or(pos.z);
    if (grounded && input && IsKeyPressed(KEY_SPACE)) {
      vel_z = 4.2;
      grounded = false;
    }
    vel_z -= 9.81 * dt;
    pos.z += vel_z * dt;
    if (pos.z <= gz || (grounded && pos.z - gz < 0.35)) {  // follow slopes and kerbs downhill
      pos.z = gz;
      vel_z = 0;
      grounded = true;
    } else {
      grounded = false;
    }
  }
  distance_walked += std::hypot(pos.x - before.x, pos.y - before.y);
}

Camera3D Player::camera(float fov_deg, float third_person_dist) const {
  Camera3D c{};
  const rj::geo::Vec3d eye = eyeEnu();
  const rj::geo::Vec3d f = forwardEnu();
  if (camera_mode == 1) {
    const double k = third_person_dist / 4.5;
    const rj::geo::Vec3d back{eye.x - f.x * third_person_dist, eye.y - f.y * third_person_dist,
                              eye.z - f.z * third_person_dist + 0.6 * k};
    c.position = enuToRl(back);
    c.target = enuToRl(eye);
  } else {
    c.position = enuToRl(eye);
    c.target = enuToRl(eye + f);
  }
  c.up = {0, 1, 0};
  c.fovy = fov_deg;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

}  // namespace rjc
