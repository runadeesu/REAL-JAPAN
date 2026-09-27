#pragma once
// Forward PBR renderer (OpenGL 3.3 via raylib/rlgl).
//   * GGX specular + Fresnel, hemisphere ambient, analytic-sky reflections, per-material
//     shading (asphalt, paving, paint, metal, glass, emissive lamps ...)
//   * real astronomical sun with atmospheric extinction, weather-driven sky, cumulus + cirrus
//     layers, cloud shadows, aerial perspective / height haze, stars
//   * two-cascade PCF sun shadows (near 70 m sharp + far 460 m)
//   * up to 24 nearby artificial lights (real PLATEAU street-light heads, shops)
//   * wet surfaces, puddles and ripples driven by the weather simulation
//   * post chain: SSAO, screen-space reflections, bloom, auto exposure, colour temperature /
//     grading, FXAA
// NOT implemented (honest list): ray-traced GI/reflections, volumetric light shafts, virtual
// texturing, motion blur. See docs/STATUS.md.

#include <functional>
#include <vector>

#include "game/aircraft.hpp"
#include "game/crowd.hpp"
#include "game/ferries.hpp"
#include "game/pedestrians.hpp"
#include "game/road_markings.hpp"
#include "game/trains.hpp"
#include "game/traffic_signals.hpp"
#include "game/weather.hpp"
#include "raylib.h"
#include "render/aircraft.hpp"
#include "render/humans.hpp"
#include "render/ships.hpp"
#include "render/textures.hpp"
#include "render/trains.hpp"
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
  float fog_far = 0.00003f;  // the weather's own haze (far view)
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
  static constexpr size_t kMaxLights = 32;  // must match the lit shader arrays
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
  // Rear view for the car's mirrors: renders `scene` (3D draw calls) from `rear` into a small
  // target. Call before beginScene(). The cockpit then shows it (mirror-flipped) on the glass.
  void renderMirror(const Camera3D& rear, const Lighting& L, float time_s, const std::function<void()>& scene);
  void invalidateMirror() { mirror_ok_ = false; }
  void setLights(const std::vector<PointLight>& lights);
  // Fictional country: snow-potential map (tex.id 0: none) with its raylib (x, z) -> uv mapping, and
  // the season (lying snow 0..1, rice paddies 0..3, leaves 0..2; see the lit shader)
  // Far view beyond the streamed cells (drawn first, in its own depth range; see App::renderScene)
  void drawCellSeas(const World& world);  // sea surfaces of the streamed cells (with the far view)
  void drawFarView(const class FarView& far, const World& world, const Camera3D& cam, float sea_y, float fog_density);
  void setSnowMap(Texture2D tex, Vector3 u, Vector3 v) {
    snow_tex_ = tex;
    snow_u_ = u;
    snow_v_ = v;
  }
  void setSeason(float snow, float crop, float leaf) {
    season_snow_ = snow;
    season_crop_ = crop;
    season_leaf_ = leaf;
  }
  // neutral_floor: flat stand-in plane outside data coverage (off underground, where it would cut through).
  void drawWorld(const Camera3D& cam, const World& world, bool photo_textures, bool neutral_floor = true);
  void drawMeshMat(const Mesh& m, const Matrix& model, int material, Color tint, Vector3 emissive = {0, 0, 0});
  void drawPlayerBody(const Vector3& feet, float yaw_rad);
  void drawPedestrians(const Pedestrians& peds, float rain = 0.0f);
  void drawInterior(const Interior& in);
  void drawFacades(const FacadeDetail& f);
  void drawMarkings(const RoadMarkings& m);
  // Open sea to the horizon at raylib height sea_y (fictional island world).
  void drawOcean(const Camera3D& cam, float sea_y);
  // ride_train / ride_car: the car the player sits in (drawn without glass, with its interior)
  void drawTrains(const Trains& trains, const Camera3D& cam, int ride_train, int ride_car);
  void drawCrowd(const std::vector<CrowdPerson>& people);
  // Station name boards on the platforms (generic design: the name, its reading and romanisation,
  // the neighbouring stations on a band in the line colour). Built once into render textures with
  // the UI font (call outside the scene pass).
  bool stationSignsBuilt() const { return signs_built_; }
  void buildStationSigns(const Trains& trains, const Font& font);
  void drawStationSigns(const Trains& trains, const Camera3D& cam);
  // LED departure boards hung over each platform (next train, how soon, a scrolling notice);
  // re-rendered only when the text changes (call outside the scene pass)
  void setDepartureBoard(int station, int side, const std::string& type, const std::string& dest, const std::string& when,
                         const std::string& notice, const Font& font);
  void drawDepartureBoards(const Trains& trains, const Camera3D& cam, float time_s);
  // information displays inside the car ridden (commuter: over every door; Shinkansen: on the end
  // wall): the next or current station
  void setCarDisplay(const std::string& text, const Font& font);
  void drawCarDisplay(const Trains& trains, int ride_train, int ride_car);
  void drawShips(const Ferries& ferries, const Camera3D& cam, const Lighting& L);
  // Aircraft: scheduled jets (ride_jet: drawn from the cabin) and the light aircraft (cockpit
  // instruments when flown from the seat).
  struct FlightView {
    bool cockpit = false;
    float kt = 0, alt_ft = 0, vs_fpm = 0, heading = 0, turn_dps = 0, pitch = 0, roll = 0, elevator = 0, aileron = 0;
  };
  void drawAircraft(const Aviation& av, const Camera3D& cam, const Lighting& L, int ride_jet, const FlightView* fv);
  void drawSignals(const TrafficSignals& ts, const Camera3D& cam);
  // extra: the player's car; with a cockpit view it is drawn from the driver's seat (interior, gauges)
  struct CockpitView {
    float kmh = 0, rpm = 0, steer = 0;  // steer: road-wheel angle (rad)
  };
  // extra: the player's car (driven or parked); extra_driven: the player sits in it (drawn at the
  // wheel in the chase view). Traffic near the camera has a driver at the (right-hand) wheel.
  void drawVehicles(const Traffic& traffic, const Camera3D& cam, const Lighting& L, const Vehicle* extra = nullptr,
                    const CockpitView* cockpit = nullptr, bool extra_driven = false);
  void vehicleCasters(const Traffic& traffic, const Camera3D& cam, std::vector<Caster>& out, const Vehicle* extra = nullptr) const;
  void drawRain(const Camera3D& cam, const Lighting& L, float time_s);
  void endScene(const Camera3D& cam, const Lighting& L, float time_s);
  void beginTransparent();  // re-enable blending (rain, particles) after the opaque pass

  float exposure() const { return exposure_; }
  void setExposureOverride(float e) { exposure_override_ = e; }
  static void clearDepth();  // depth only (between the far view and the near scene)
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
  HumanModels humans_;
  std::vector<RenderTexture2D> signs_;  // per station
  bool signs_built_ = false;
  struct DepartureBoard {
    int station = -1, side = 0;
    std::string key;  // text currently shown
    RenderTexture2D rt{};
    float notice_w = 0;  // width of the notice line (scrolls when wider than the board)
  };
  std::vector<DepartureBoard> boards_;
  RenderTexture2D car_display_{};
  std::string car_display_key_;
  Mesh ocean_{};
  TrainModels train_models_;
  ShipModels ship_models_;
  AircraftModels aircraft_models_;
  void drawHuman(const Mesh& m, const Matrix& model, Color top, Color bottom, Color skin, Color hair);
  // shadows: 0 = near cascade, 1 = far cascade
  RenderTexture2D shadow_[2]{};
  int shadow_res_[2] = {2048, 4096};
  float shadow_extent_[2] = {70.0f, 460.0f};
  Matrix light_vp_[2]{};
  bool shadow_valid_ = false;
  // post targets
  RenderTexture2D scene_{};
  RenderTexture2D mirror_{};
  bool mirror_ok_ = false;
  Vector2 sky_res_override_{0, 0};
  void drawMirrorGlass(const Vector3 c[4], float u0, float u1);
  RenderTexture2D ao_{}, ao_blur_{}, bright_rt_{}, bloom_a_{}, bloom_b_{}, lum_{}, ssr_rt_{};
  int tw_ = 0, th_ = 0;
  bool post_active_ = false;
  RenderOptions opt_{};
  Texture2D snow_tex_{};
  Vector3 snow_u_{0, 0, 0}, snow_v_{0, 0, 0};
  float season_snow_ = 0.0f, season_crop_ = 2.0f, season_leaf_ = 0.0f;
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
