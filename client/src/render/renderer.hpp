#pragma once
// Forward renderer for Phase 0/1 (OpenGL 3.3 via raylib/rlgl).
//   * sun direction and colour from the real astronomical sun position
//   * physically-motivated sky gradient + sun disc, distance fog
//   * directional shadow map (PCF) around the camera
//   * linear-space lighting with ACES-style tone mapping
// The photoreal pipeline (GI, RT, virtualised geometry) is a later phase and
// is NOT implemented here; see docs/ARCHITECTURE.md (Rendering).

#include "raylib.h"
#include "game/pedestrians.hpp"
#include "world/world.hpp"

namespace rjc {

struct Lighting {
  Vector3 sun_dir{0, 1, 0};  // towards the sun, raylib space
  float sun_elevation_deg = 45.0f;
  Vector3 sun_color{1, 1, 1};
  Vector3 sky_zenith{0.3f, 0.5f, 0.9f};
  Vector3 sky_horizon{0.7f, 0.8f, 0.95f};
  Vector3 ambient_sky{0.3f, 0.35f, 0.45f};
  Vector3 ambient_ground{0.15f, 0.14f, 0.12f};
  float fog_density = 0.0006f;
};

Lighting lightingForSun(float elevation_deg, float azimuth_deg, float view_distance_m);

class Renderer {
 public:
  bool init();
  void shutdown();

  void renderShadowMap(const Camera3D& cam, const World& world, const Lighting& L);
  void drawSky(const Camera3D& cam, const Lighting& L, float aspect);
  void drawWorld(const Camera3D& cam, const World& world, const Lighting& L, bool shadows, bool photo_textures);
  void drawPlayerBody(const Vector3& feet, float yaw_rad, const Lighting& L);
  void drawPedestrians(const Pedestrians& peds);

  static void setClipPlanes(float near_m, float far_m);

 private:
  void applyLightingUniforms(Shader& s, const Camera3D& cam, const Lighting& L, bool shadows);
  Shader lit_{}, sky_{}, depth_{};
  RenderTexture2D shadow_{};
  int shadow_res_ = 4096;
  Matrix light_vp_{};
  bool shadow_valid_ = false;
  Material mat_{};
  Material mat_depth_{};
  Mesh plane_{};
  Mesh legs_{};
  Mesh torso_{};
  Mesh body_{};
  Mesh head_{};
  bool ready_ = false;
};

}  // namespace rjc
