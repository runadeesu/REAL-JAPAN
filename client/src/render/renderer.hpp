#pragma once
// Forward PBR renderer (OpenGL 3.3 via raylib/rlgl).
//   * GGX specular + Fresnel, hemisphere ambient, analytic-sky reflections, per-material
//     shading (asphalt, paving, paint, metal, glass, emissive lamps ...)
//   * real astronomical sun with atmospheric extinction, weather-driven sky, cumulus + cirrus
//     layers, cloud shadows, aerial perspective / height haze, stars
//   * two-cascade PCF sun shadows (near 70 m sharp + far 460 m)
//   * up to 24 nearby artificial lights (real PLATEAU street-light heads, shops)
//   * wet surfaces, puddles and ripples driven by the weather simulation
//   * post chain: SSAO, bloom, auto exposure, colour temperature / grading, FXAA
// NOT implemented (honest list): ray-traced GI/reflections, screen-space reflections,
// volumetric light shafts, virtual texturing. See docs/STATUS.md.

#include <vector>

#include "game/pedestrians.hpp"
#include "game/traffic_signals.hpp"
#include "game/weather.hpp"
#include "raylib.h"
#include "render/textures.hpp"
#include "render/vehicles.hpp"
#include "world/facade.hpp"
#include "world/world.hpp"

namespace rjc {

struct Lighting {
  Vector3 sun_dir{0, 1, 0};  // towards the sun, raylib space
  float sun_elevation_deg = 45.0f;
  Vector3 sun_color{1, 1, 1};  // irradiance incl. atmospheric extinction and cloud attenuation
  Vector3 sky_zenith{0.08f, 0.22f, 0.62f};
  Vector3 sky_horizon{0.55f, 0.62f, 0.72f};
  Vector3 haze{0.6f, 0.64f, 0.7f};
  Vector3 ambient_sky{0.3f, 0.36f, 0.46f};
  Vector3 ambient_ground{0.16f, 0.15f, 0.13f};
  float fog_density = 0.0004f;
  float cloud_cover = 0.2f;
  Vector2 cloud_offset{0, 0};
  float wetness = 0.0f;
  float rain = 0.0f;       // 0..1 visual rain intensity
  float night = 0.0f;      // 0 day .. 1 artificial lights fully on
  float wind = 0.3f;       // 0..1.5 foliage sway
  float stars = 0.0f;
  Vector3 occupancy{0.5f, 0.3f, 0.8f};  // lit-window fractions: office, residential, shop
  float sun_visible = 1.0f;
  float indoor = 0.0f;
  float lightning = 0.0f;
  Vector3 white_balance{1, 1, 1};
  float saturation = 1.05f;
  float contrast = 1.04f;
};

Lighting computeLighting(float sun_elevation_deg, float sun_azimuth_deg, const WeatherParams& w, float wetness,
                         float lightning, Vector2 cloud_offset);
Lighting indoorLighting();
Lighting lerpLighting(const Lighting& a, const Lighting& b, float t);

struct PointLight {
  Vector3 pos;  // raylib space
  float range;
  Vector3 color;
};

struct RenderOptions {
  bool shadows = true;
  bool post = true;
  bool ssao = true;
  bool bloom = true;
};

class Renderer {
 public:
  bool init();
  void shutdown();

  // Frame structure: shadows -> beginScene -> sky + 3D draws -> endScene (post -> backbuffer).
  struct Caster {
    const Mesh* mesh;
    Matrix model;
  };
  void renderShadowMaps(const Camera3D& cam, const World& world, const Lighting& L, const std::vector<Caster>& extra_casters = {});
  void beginScene(const RenderOptions& o, const Lighting& L, const Camera3D& cam, float time_s);
  void drawSky(const Camera3D& cam, const Lighting& L, float aspect);
  void setLights(const std::vector<PointLight>& lights);
  // neutral_floor: flat stand-in plane outside data coverage (off underground, where it would cut through).
  void drawWorld(const Camera3D& cam, const World& world, bool photo_textures, bool neutral_floor = true);
  void drawMeshMat(const Mesh& m, const Matrix& model, int material, Color tint, Vector3 emissive = {0, 0, 0});
  void drawPlayerBody(const Vector3& feet, float yaw_rad);
  void drawPedestrians(const Pedestrians& peds);
  void drawInterior(const Interior& in);
  void drawFacades(const FacadeDetail& f);
  void drawSignals(const TrafficSignals& ts, const Camera3D& cam);
  void drawVehicles(const Traffic& traffic, const Camera3D& cam, const Lighting& L);
  void vehicleCasters(const Traffic& traffic, const Camera3D& cam, std::vector<Caster>& out) const;
  void drawRain(const Camera3D& cam, const Lighting& L, float time_s);
  void endScene(const Camera3D& cam, const Lighting& L, float time_s);
  void beginTransparent();  // re-enable blending (rain, particles) after the opaque pass

  float exposure() const { return exposure_; }
  void setExposureOverride(float e) { exposure_override_ = e; }
  static void setClipPlanes(float near_m, float far_m);
  int drawCalls() const { return draw_calls_; }
  long long triangles() const { return triangles_; }

 private:
  void applyFrameUniforms(const Camera3D& cam, const Lighting& L, float time_s);
  void bindGlobalTextures();
  void ensureTargets();
  void releaseTargets();
  void fullscreen(Shader& sh, const Texture2D& src, RenderTexture2D& dst, bool flip_src);

  Shader lit_{}, sky_{}, depth_{}, ssao_{}, blur_{}, bright_{}, composite_{}, ssr_{};
  Material mat_{}, mat_depth_{};
  DetailTextures tex_;
  Texture2D leaf_tex_{};
  VehicleModels vehicles_;
  // shadows: 0 = near cascade, 1 = far cascade
  RenderTexture2D shadow_[2]{};
  int shadow_res_[2] = {2048, 4096};
  float shadow_extent_[2] = {70.0f, 460.0f};
  Matrix light_vp_[2]{};
  bool shadow_valid_ = false;
  // post targets
  RenderTexture2D scene_{};
  RenderTexture2D ao_{}, ao_blur_{}, bright_rt_{}, bloom_a_{}, bloom_b_{}, lum_{}, ssr_rt_{};
  int tw_ = 0, th_ = 0;
  bool post_active_ = false;
  RenderOptions opt_{};
  float exposure_ = 1.0f;
  float exposure_override_ = 0.0f;
  int frame_ = 0;
  int draw_calls_ = 0;
  long long triangles_ = 0;
  Mesh plane_{}, legs_{}, torso_{}, body_{}, head_{}, lens_{}, pedlens_{};
  bool ready_ = false;
  Lighting frame_L_{};
};

}  // namespace rjc
