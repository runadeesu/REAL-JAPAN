#include "render/renderer.hpp"

#include "game/traffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "raymath.h"
#include "game/driving.hpp"
#include "game/station_names.hpp"
#include "render/farview.hpp"
#include "render/gpu_mesh.hpp"
#include "render/foliage.hpp"
#include "render/shaders.hpp"
#include "rlgl.h"
#include "world/coords.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

// Texture units reserved for per-frame textures (slot 0 = material diffuse, 1-4 = rlgl batch).
constexpr int kSlotAO = 5, kSlotNoise = 6, kSlotAsphalt = 7, kSlotAsphaltN = 8, kSlotPaving = 9, kSlotPavingN = 10,
              kSlotShadow0 = 11, kSlotShadow1 = 12, kSlotLand = 13, kSlotSnow = 14;

float g_near = 0.3f, g_far = 3000.0f;

// Interior lighting of vehicles the player rides in (neutral LED strips in trains, warmer cabin
// lighting in the jet); added to the sky ambient, which the shell's shadow leaves too dark inside.
const Vector3 kCabinLight{0.85f, 0.85f, 0.80f}, kJetCabinLight{0.72f, 0.68f, 0.60f};

Vector3 lerp3(Vector3 a, Vector3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
Vector3 mul3(Vector3 a, Vector3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

void setI(Shader& s, const char* n, int v) { SetShaderValue(s, GetShaderLocation(s, n), &v, SHADER_UNIFORM_INT); }
void setF(Shader& s, const char* n, float v) { SetShaderValue(s, GetShaderLocation(s, n), &v, SHADER_UNIFORM_FLOAT); }
void set2(Shader& s, const char* n, Vector2 v) { SetShaderValue(s, GetShaderLocation(s, n), &v, SHADER_UNIFORM_VEC2); }
void set3(Shader& s, const char* n, Vector3 v) { SetShaderValue(s, GetShaderLocation(s, n), &v, SHADER_UNIFORM_VEC3); }

}  // namespace

// OpenGL 1.1 entry points from the system library (exported by opengl32.dll / libGL): rlgl only
// clears colour and depth together, and lets the driver pick the depth texture's precision.
extern "C" {
void glClear(unsigned int mask);
void glGenTextures(int n, unsigned int* textures);
void glBindTexture(unsigned int target, unsigned int texture);
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border, unsigned int format,
                  unsigned int type, const void* pixels);
void glTexParameteri(unsigned int target, unsigned int pname, int param);
}

namespace {

// Scene depth as a sized 24-bit texture: an unsized request may get 16 bits, whose steps at
// 50-150 m (the ground seen from a train or a low flight) show up as bands in the SSAO.
unsigned int loadDepthTexture24(int w, int h) {
  constexpr unsigned int kTex2D = 0x0DE1, kDepth = 0x1902, kDepth24 = 0x81A6, kUInt = 0x1405;
  constexpr unsigned int kMin = 0x2801, kMag = 0x2800, kWrapS = 0x2802, kWrapT = 0x2803, kNearest = 0x2600, kClampEdge = 0x812F;
  unsigned int id = 0;
  glGenTextures(1, &id);
  if (!id) return rlLoadTextureDepth(w, h, false);
  glBindTexture(kTex2D, id);
  glTexImage2D(kTex2D, 0, static_cast<int>(kDepth24), w, h, 0, kDepth, kUInt, nullptr);
  glTexParameteri(kTex2D, kMin, kNearest);
  glTexParameteri(kTex2D, kMag, kNearest);
  glTexParameteri(kTex2D, kWrapS, kClampEdge);
  glTexParameteri(kTex2D, kWrapT, kClampEdge);
  glBindTexture(kTex2D, 0);
  return id;
}

RenderTexture2D makeTarget(int w, int h, bool depth_texture) {
  RenderTexture2D t{};
  t.id = rlLoadFramebuffer();
  if (!t.id) return t;
  rlEnableFramebuffer(t.id);
  t.texture.id = rlLoadTexture(nullptr, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  t.texture.width = w;
  t.texture.height = h;
  t.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  t.texture.mipmaps = 1;
  if (depth_texture) {
    t.depth.id = loadDepthTexture24(w, h);
    t.depth.format = 19;
  } else {
    t.depth.id = rlLoadTextureDepth(w, h, true);  // renderbuffer
    t.depth.format = 19;
  }
  t.depth.width = w;
  t.depth.height = h;
  t.depth.mipmaps = 1;
  rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
  rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, depth_texture ? RL_ATTACHMENT_TEXTURE2D : RL_ATTACHMENT_RENDERBUFFER, 0);
  if (!rlFramebufferComplete(t.id)) TraceLog(LOG_WARNING, "RJ: framebuffer %dx%d incomplete", w, h);
  rlDisableFramebuffer();
  SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
  return t;
}

void freeTarget(RenderTexture2D& t) {
  if (!t.id) return;
  UnloadRenderTexture(t);  // frees colour texture, depth (texture or renderbuffer) and the FBO
  t = RenderTexture2D{};
}

}  // namespace

// ---------------------------------------------------------------------------
Lighting computeLighting(float el, float az, const WeatherParams& w, float wetness, float lightning, Vector2 cloud_offset) {
  Lighting L;
  const float e = el * DEG2RAD, a = az * DEG2RAD;
  const rj::geo::Vec3d enu{std::cos(e) * std::sin(a), std::cos(e) * std::cos(a), std::sin(e)};
  L.sun_dir = Vector3Normalize(enuToRl(enu));
  L.sun_elevation_deg = el;
  const float cc = std::clamp(w.cloud_cover, 0.0f, 1.0f);
  const float fog = std::clamp(w.fog, 0.0f, 1.0f);
  // Direct sun: Kasten-Young air mass with a hazy mid-latitude urban extinction.
  const float elc = std::max(el, 0.3f);
  const float m = 1.0f / (std::sin(elc * DEG2RAD) + 0.50572f * std::pow(elc + 6.07995f, -1.6364f));
  const Vector3 tau{0.050f + 0.05f * fog, 0.090f + 0.05f * fog, 0.190f + 0.05f * fog};
  const Vector3 trans{std::exp(-tau.x * m), std::exp(-tau.y * m), std::exp(-tau.z * m)};
  const float up = smooth(-1.2f, 3.0f, el);
  const float direct = up * (1.0f - 0.9f * cc) * (1.0f - 0.95f * fog);
  L.sun_color = Vector3Scale(mul3(Vector3{1.0f, 0.97f, 0.94f}, trans), 2.3f * direct);
  // Sky radiance (zenith / horizon) through the day.
  const float day = smooth(-4.0f, 20.0f, el);
  const float twi = smooth(-14.0f, 2.0f, el);
  const Vector3 zen_day{0.07f, 0.18f, 0.52f}, hor_day{0.50f, 0.58f, 0.68f};
  const Vector3 zen_dusk{0.09f, 0.10f, 0.26f}, hor_dusk{0.80f, 0.46f, 0.30f};
  const Vector3 zen_night{0.009f, 0.009f, 0.013f}, hor_night{0.070f, 0.056f, 0.050f};  // Tokyo sky glow (warm, light-polluted)
  Vector3 zen = lerp3(lerp3(zen_night, zen_dusk, twi), zen_day, day);
  Vector3 hor = lerp3(lerp3(hor_night, hor_dusk, twi), hor_day, day);
  const float bright = 0.03f + 0.97f * day + 0.15f * twi * (1.0f - day);
  const Vector3 grey = Vector3Scale(Vector3{0.44f, 0.455f, 0.48f}, bright);
  zen = lerp3(zen, Vector3Scale(grey, 0.9f), cc);
  hor = lerp3(hor, grey, cc);
  hor = lerp3(hor, Vector3Scale(Vector3{0.62f, 0.63f, 0.64f}, bright), fog);
  zen = lerp3(zen, Vector3Scale(Vector3{0.60f, 0.61f, 0.62f}, bright), fog * 0.8f);
  L.sky_zenith = zen;
  L.sky_horizon = hor;
  L.haze = lerp3(hor, Vector3Scale(Vector3{0.58f, 0.60f, 0.62f}, bright), 0.4f);
  // Ambient irradiance: clear-sky blue, overcast grey (brighter, softer), dusk, night + city glow.
  const Vector3 amb_day{0.24f, 0.30f, 0.40f}, amb_ovc{0.36f, 0.37f, 0.39f}, amb_dusk{0.09f, 0.085f, 0.11f};
  const Vector3 amb_night{0.032f, 0.029f, 0.030f};  // sky glow + light scattered from lit streets
  L.ambient_sky = lerp3(lerp3(amb_night, amb_dusk, twi), lerp3(amb_day, amb_ovc, cc), day);
  // Ground/building bounce: warm, driven by sunlight on the city.
  const Vector3 bounce = Vector3Add(Vector3Scale(L.sun_color, 0.20f * std::max(0.0f, std::sin(e))), Vector3Scale(L.ambient_sky, 0.45f));
  L.ambient_ground = mul3(bounce, Vector3{1.0f, 0.93f, 0.84f});
  if (lightning > 0.0f) {
    const Vector3 flash = Vector3Scale(Vector3{0.8f, 0.82f, 1.0f}, lightning * 1.6f);
    L.ambient_sky = Vector3Add(L.ambient_sky, flash);
    L.sky_zenith = Vector3Add(L.sky_zenith, Vector3Scale(flash, 0.6f));
    L.sky_horizon = Vector3Add(L.sky_horizon, Vector3Scale(flash, 0.8f));
  }
  L.fog_density = 0.00032f + 0.00045f * cc + 0.00005f * std::min(w.rain_mm_h, 30.0f) + 0.009f * fog * fog;
  L.cloud_cover = cc;
  L.cloud_offset = cloud_offset;
  L.wetness = wetness;
  L.rain = std::clamp(w.rain_mm_h / 12.0f, 0.0f, 1.0f);
  L.night = std::max(1.0f - smooth(-5.0f, 4.0f, el), 0.6f * cc * (1.0f - smooth(3.0f, 14.0f, el)));
  L.stars = (1.0f - smooth(-16.0f, -7.0f, el)) * (1.0f - cc);
  L.sun_visible = up * (1.0f - 0.97f * cc) * (1.0f - fog);
  L.lightning = lightning;
  // Grading: golden hour warm, overcast cool and flatter, night richer.
  const float golden = up * (1.0f - smooth(4.0f, 18.0f, el));
  L.white_balance = lerp3(lerp3(Vector3{1.0f, 1.0f, 1.0f}, Vector3{1.05f, 1.0f, 0.93f}, golden), Vector3{0.97f, 1.0f, 1.035f}, cc * day);
  L.saturation = 1.06f - 0.12f * cc * day + 0.06f * L.night;
  L.contrast = 1.05f - 0.05f * cc - 0.06f * fog;
  return L;
}

Lighting indoorLighting() {
  // Artificial light for underground spaces: bright diffuse ceiling light, no sky.
  Lighting L;
  L.sun_dir = Vector3Normalize(Vector3{0.15f, 1.0f, 0.1f});
  L.sun_elevation_deg = -90.0f;
  L.sun_color = {0.45f, 0.45f, 0.43f};
  L.ambient_sky = {0.62f, 0.62f, 0.60f};
  L.ambient_ground = {0.34f, 0.33f, 0.31f};
  L.sky_zenith = L.sky_horizon = L.haze = {0.30f, 0.30f, 0.31f};
  L.fog_density = 0.0025f;
  L.cloud_cover = 1.0f;
  L.night = 1.0f;
  L.sun_visible = 0.0f;
  L.indoor = 1.0f;
  L.white_balance = {1.0f, 0.99f, 0.96f};
  return L;
}

Lighting lerpLighting(const Lighting& a, const Lighting& b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  Lighting L = t < 0.5f ? a : b;
  L.sun_dir = Vector3Normalize(Vector3Lerp(a.sun_dir, b.sun_dir, t));
  L.sun_color = Vector3Lerp(a.sun_color, b.sun_color, t);
  L.sky_zenith = Vector3Lerp(a.sky_zenith, b.sky_zenith, t);
  L.sky_horizon = Vector3Lerp(a.sky_horizon, b.sky_horizon, t);
  L.haze = Vector3Lerp(a.haze, b.haze, t);
  L.ambient_sky = Vector3Lerp(a.ambient_sky, b.ambient_sky, t);
  L.ambient_ground = Vector3Lerp(a.ambient_ground, b.ambient_ground, t);
  L.fog_density = a.fog_density + (b.fog_density - a.fog_density) * t;
  L.night = a.night + (b.night - a.night) * t;
  L.indoor = a.indoor + (b.indoor - a.indoor) * t;
  L.sun_visible = a.sun_visible + (b.sun_visible - a.sun_visible) * t;
  L.white_balance = Vector3Lerp(a.white_balance, b.white_balance, t);
  return L;
}

// ---------------------------------------------------------------------------
namespace {
// The shaders are written for desktop GLSL 3.30; OpenGL ES 3.0 (Android) takes the same code with
// its own version line and default precisions.
Shader loadShader(const char* vs, const char* fs) {
#if defined(RJ_GLES)
  auto adapt = [](const char* src, std::string& out) -> const char* {
    if (!src) return nullptr;
    out = src;
    const std::string v = "#version 330";
    if (const auto p = out.find(v); p != std::string::npos)
      out.replace(p, v.size(), "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;");
    return out.c_str();
  };
  std::string a, b;
  return LoadShaderFromMemory(adapt(vs, a), adapt(fs, b));
#else
  return LoadShaderFromMemory(vs, fs);
#endif
}
}  // namespace

bool Renderer::init() {
  lit_ = loadShader(shaders::kLitVs, shaders::kLitFs);
  depth_ = loadShader(shaders::kDepthVs, shaders::kDepthFs);
  sky_ = loadShader(shaders::kSkyVs, shaders::kSkyFs);
  ssao_ = loadShader(nullptr, shaders::kSsaoFs);
  blur_ = loadShader(nullptr, shaders::kBlurFs);
  bright_ = loadShader(nullptr, shaders::kBrightFs);
  composite_ = loadShader(nullptr, shaders::kCompositeFs);
  ssr_ = loadShader(nullptr, shaders::kSsrFs);
  for (Shader* s : {&lit_, &depth_, &sky_, &ssao_, &blur_, &bright_, &composite_, &ssr_})
    if (!IsShaderValid(*s)) {
      TraceLog(LOG_ERROR, "RJ: shader compilation failed");
      return false;
    }
  lit_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(lit_, "matModel");
  lit_.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(lit_, "matNormal");
  lit_.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(lit_, "viewPos");
  lit_.locs[SHADER_LOC_VERTEX_TEXCOORD02] = GetShaderLocationAttrib(lit_, "vertexTexCoord2");
  mat_ = LoadMaterialDefault();
  mat_.shader = lit_;
  mat_depth_ = LoadMaterialDefault();
  mat_depth_.shader = depth_;
  // Fixed sampler units.
  setI(lit_, "texAO", kSlotAO);
  setI(lit_, "texNoise", kSlotNoise);
  setI(lit_, "texAsphalt", kSlotAsphalt);
  setI(lit_, "texAsphaltN", kSlotAsphaltN);
  setI(lit_, "texPaving", kSlotPaving);
  setI(lit_, "texPavingN", kSlotPavingN);
  setI(lit_, "shadowMap0", kSlotShadow0);
  setI(lit_, "shadowMap1", kSlotShadow1);
  setI(lit_, "texLand", kSlotLand);
  setI(lit_, "texSnow", kSlotSnow);
  setI(lit_, "landOn", 0);
  setI(lit_, "materialOverride", -1);
  setI(sky_, "texNoise", kSlotNoise);
  if (!tex_.generate(512)) return false;
  leaf_tex_ = generateLeafTexture(512);
  vehicles_.build();
  humans_.build();
  train_models_.build();
  ship_models_.build();
  aircraft_models_.build();
  lit_.locs[SHADER_LOC_MATRIX_VIEW] = GetShaderLocation(lit_, "matView");
  lit_.locs[SHADER_LOC_MATRIX_PROJECTION] = GetShaderLocation(lit_, "matProjection");

  for (int c = 0; c < 2; ++c) {
    RenderTexture2D& s = shadow_[c];
    s.id = rlLoadFramebuffer();
    s.texture.width = s.texture.height = shadow_res_[c];
    if (!s.id) continue;
    rlEnableFramebuffer(s.id);
    s.depth.id = rlLoadTextureDepth(shadow_res_[c], shadow_res_[c], false);
    s.depth.width = s.depth.height = shadow_res_[c];
    s.depth.format = 19;
    s.depth.mipmaps = 1;
    rlFramebufferAttach(s.id, s.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(s.id)) TraceLog(LOG_WARNING, "RJ: shadow framebuffer incomplete");
    rlDisableFramebuffer();
  }
  plane_ = GenMeshPlane(12000.0f, 12000.0f, 1, 1);
  ocean_ = GenMeshPlane(40000.0f, 40000.0f, 80, 80);
  {
    // the open sea to the horizon for the far view: a disc following the Earth's curvature below
    // the camera (the streamed cells' own seas and the far terrain follow it too, so they meet
    // without the flat plane rising over the far coasts or crossing the near seas)
    constexpr int kSeg = 72, kRings = 40;
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned short> idx;
    std::vector<float> radii = {0.0f};
    for (int r = 1; r <= kRings; ++r) radii.push_back(60.0f * std::pow(1.2f, static_cast<float>(r)) - 60.0f);
    for (float r : radii)
      for (int k = 0; k < kSeg; ++k) {
        const float a = 6.2831853f * k / kSeg;
        pos.insert(pos.end(), {r * std::cos(a), -r * r / (2.0f * 6371000.0f), r * std::sin(a)});
        nrm.insert(nrm.end(), {0.0f, 1.0f, 0.0f});
        uv.insert(uv.end(), {0.0f, 0.0f});
      }
    for (int r = 0; r + 1 < static_cast<int>(radii.size()); ++r)
      for (int k = 0; k < kSeg; ++k) {
        const auto a = static_cast<unsigned short>(r * kSeg + k), b = static_cast<unsigned short>(r * kSeg + (k + 1) % kSeg);
        const auto c = static_cast<unsigned short>((r + 1) * kSeg + k), d = static_cast<unsigned short>((r + 1) * kSeg + (k + 1) % kSeg);
        idx.insert(idx.end(), {a, d, c, a, b, d});  // (counter-clockwise seen from above: the lit shader flips the normal of back faces)
      }
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.indices = idx.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    ocean_curved_ = m;
  }
  body_ = GenMeshCylinder(0.25f, 1.35f, 12);
  legs_ = GenMeshCylinder(0.15f, 0.82f, 8);
  torso_ = GenMeshCylinder(0.21f, 0.64f, 10);
  head_ = GenMeshSphere(0.14f, 10, 12);
  lens_ = GenMeshCylinder(0.125f, 0.03f, 16);     // 300 mm vehicle signal lens
  pedlens_ = GenMeshCube(0.22f, 0.22f, 0.03f);     // pedestrian signal panel
  unit_box_ = GenMeshCube(1.0f, 1.0f, 1.0f);
  ready_ = true;
  return true;
}

void Renderer::shutdown() {
  if (!ready_) return;
  releaseTargets();
  freeTarget(mirror_);
  for (auto& rt : signs_) UnloadRenderTexture(rt);
  signs_.clear();
  for (auto& b : boards_) UnloadRenderTexture(b.rt);
  boards_.clear();
  if (car_display_.id) UnloadRenderTexture(car_display_);
  car_display_ = RenderTexture2D{};
  signs_built_ = false;
  for (Mesh* m : {&plane_, &ocean_, &ocean_curved_, &body_, &legs_, &torso_, &head_, &lens_, &pedlens_, &unit_box_}) UnloadMesh(*m);
  for (auto& s : shadow_)
    if (s.id) {
      rlUnloadFramebuffer(s.id);
      if (s.depth.id) rlUnloadTexture(s.depth.id);
    }
  tex_.unload();
  if (leaf_tex_.id) UnloadTexture(leaf_tex_);
  vehicles_.unload();
  humans_.unload();
  train_models_.unload();
  ship_models_.unload();
  aircraft_models_.unload();
  mat_.shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
  mat_depth_.shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
  for (Shader* s : {&lit_, &depth_, &sky_, &ssao_, &blur_, &bright_, &composite_, &ssr_}) UnloadShader(*s);
  ready_ = false;
}

void Renderer::clearDepth() {
  rlDrawRenderBatchActive();
  glClear(0x00000100u);  // GL_DEPTH_BUFFER_BIT
}

void Renderer::setClipPlanes(float near_m, float far_m) {
  g_near = near_m;
  g_far = far_m;
  rlSetClipPlanes(near_m, far_m);
}

void Renderer::releaseTargets() {
  for (RenderTexture2D* t : {&scene_, &ao_, &ao_blur_, &bright_rt_, &bloom_a_, &bloom_b_, &lum_, &ssr_rt_}) freeTarget(*t);
  tw_ = th_ = 0;
}

void Renderer::ensureTargets() {
  const int w = std::max(1, GetRenderWidth()), h = std::max(1, GetRenderHeight());
  if (w == tw_ && h == th_ && scene_.id) return;
  releaseTargets();
  tw_ = w;
  th_ = h;
  scene_ = makeTarget(w, h, true);
  ao_ = makeTarget(std::max(1, w / 2), std::max(1, h / 2), false);
  ao_blur_ = makeTarget(std::max(1, w / 2), std::max(1, h / 2), false);
  bright_rt_ = makeTarget(std::max(1, w / 4), std::max(1, h / 4), false);
  bloom_a_ = makeTarget(std::max(1, w / 4), std::max(1, h / 4), false);
  bloom_b_ = makeTarget(std::max(1, w / 4), std::max(1, h / 4), false);
  lum_ = makeTarget(32, 18, false);
  ssr_rt_ = makeTarget(std::max(1, w / 2), std::max(1, h / 2), false);
}

void Renderer::bindGlobalTextures() {
  auto bind = [](int slot, unsigned int id) {
    rlActiveTextureSlot(slot);
    rlEnableTexture(id);
  };
  bind(kSlotNoise, tex_.noise.id);
  bind(kSlotAsphalt, tex_.asphalt.id);
  bind(kSlotAsphaltN, tex_.asphaltN.id);
  bind(kSlotPaving, tex_.paving.id);
  bind(kSlotPavingN, tex_.pavingN.id);
  bind(kSlotAO, rlGetTextureIdDefault());
  bind(kSlotShadow0, shadow_valid_ && shadow_[0].depth.id ? shadow_[0].depth.id : rlGetTextureIdDefault());
  bind(kSlotShadow1, shadow_valid_ && shadow_[1].depth.id ? shadow_[1].depth.id : rlGetTextureIdDefault());
  bind(kSlotLand, rlGetTextureIdDefault());
  bind(kSlotSnow, snow_tex_.id ? snow_tex_.id : rlGetTextureIdDefault());
  rlActiveTextureSlot(0);
}

// ---------------------------------------------------------------------------
void Renderer::renderShadowMaps(const Camera3D& cam, const World& world, const Lighting& L,
                                const std::vector<Caster>& extra_casters) {
  shadow_valid_ = false;
  if (L.sun_elevation_deg < 1.0f || L.sun_visible < 0.05f || L.indoor > 0.5f) return;
  // Do not sample the maps while rendering into them.
  rlActiveTextureSlot(kSlotShadow0);
  rlDisableTexture();
  rlActiveTextureSlot(kSlotShadow1);
  rlDisableTexture();
  rlActiveTextureSlot(0);
  Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
  fwd.y = 0;
  fwd = Vector3Length(fwd) > 1e-4f ? Vector3Normalize(fwd) : Vector3{0, 0, -1};
  for (int c = 0; c < 2; ++c) {
    if (!shadow_[c].id) continue;
    const float extent = shadow_extent_[c];
    Vector3 center = Vector3Add(cam.position, Vector3Scale(fwd, extent * 0.3f));
    const float snap = extent / static_cast<float>(shadow_res_[c]) * 2.0f;
    center.x = std::round(center.x / snap) * snap;
    center.z = std::round(center.z / snap) * snap;
    center.y = cam.position.y - (c == 0 ? 2.0f : 20.0f);
    Camera3D lc{};
    lc.position = Vector3Add(center, Vector3Scale(L.sun_dir, 1500.0f));
    lc.target = center;
    lc.up = std::abs(L.sun_dir.y) > 0.99f ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
    lc.projection = CAMERA_ORTHOGRAPHIC;
    lc.fovy = extent;
    BeginTextureMode(shadow_[c]);
    ClearBackground(WHITE);
    // Tight depth range around the receivers so depth biases stay centimetre-scale.
    if (c == 0) rlSetClipPlanes(1150.0, 1600.0);
    else rlSetClipPlanes(850.0, 1900.0);
    BeginMode3D(lc);
    const Matrix view = rlGetMatrixModelview();
    const Matrix proj = rlGetMatrixProjection();
    rlDisableBackfaceCulling();
    const float r2 = (extent * 0.9f) * (extent * 0.9f);
    for (const auto& [code, cell] : world.cells()) {
      for (const auto& m : cell->gpu.chunks) DrawMesh(m, mat_depth_, cell->model);
      for (size_t i = 0; i < cell->gpu.detail.meshes.size(); ++i) {
        if (matIsFlat(cell->gpu.detail.mats[i]) || cell->gpu.detail.mats[i] == kMatClearGlass) continue;
        if (c == 1 && cell->gpu.detail.mats[i] == kMatFence) continue;  // thin rails: near cascade only
        DrawMesh(cell->gpu.detail.meshes[i], mat_depth_, cell->model);
      }
    }
    for (const auto& [code, cell] : world.cells()) {
      for (const auto& t : cell->gpu.detail.trees) {
        const Matrix m = MatrixMultiply(MatrixTranslate(t.base[0], t.base[1], t.base[2]), cell->model);
        if (t.bark.vaoId) DrawMesh(t.bark, mat_depth_, m);
        mat_depth_.maps[MATERIAL_MAP_DIFFUSE].texture = leaf_tex_;
        if (t.leaves.vaoId) DrawMesh(t.leaves, mat_depth_, m);
        mat_depth_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
      }
    }
    for (const Caster& ec : extra_casters) DrawMesh(*ec.mesh, mat_depth_, ec.model);
    (void)r2;
    rlEnableBackfaceCulling();
    EndMode3D();
    EndTextureMode();
    light_vp_[c] = MatrixMultiply(view, proj);
  }
  rlSetClipPlanes(g_near, g_far);
  shadow_valid_ = true;
}

void Renderer::applyFrameUniforms(const Camera3D& cam, const Lighting& L, float time_s) {
  const float expo = exposure_override_ > 0.0f ? exposure_override_ : exposure_;
  for (Shader* s : {&lit_, &sky_}) {
    set3(*s, "sunDir", L.sun_dir);
    set3(*s, "sunColor", L.sun_color);
    set3(*s, "skyZenith", L.sky_zenith);
    set3(*s, "skyHorizon", L.sky_horizon);
    set3(*s, "ambientSky", L.ambient_sky);
    set3(*s, "ambientGround", L.ambient_ground);
    set3(*s, "hazeColor", L.haze);
    setF(*s, "exposure", expo);
    setF(*s, "cloudCover", L.cloud_cover);
    set2(*s, "cloudOffset", L.cloud_offset);
  }
  set3(lit_, "viewPos", cam.position);
  setF(lit_, "fogDensity", L.fog_density);
  setF(lit_, "wetness", L.wetness);
  setF(lit_, "nightFactor", L.night);
  setF(lit_, "timeSec", time_s);
  setF(lit_, "windStrength", L.wind);
  setF(lit_, "indoor", L.indoor);
  set3(lit_, "occupancy", L.occupancy);
  const int on = shadow_valid_ && opt_.shadows ? 1 : 0;
  setI(lit_, "shadowsOn", on);
  setF(lit_, "shadowTexel0", 1.0f / static_cast<float>(shadow_res_[0]));
  setF(lit_, "shadowTexel1", 1.0f / static_cast<float>(shadow_res_[1]));
  SetShaderValueMatrix(lit_, GetShaderLocation(lit_, "lightVP0"), light_vp_[0]);
  SetShaderValueMatrix(lit_, GetShaderLocation(lit_, "lightVP1"), light_vp_[1]);
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  set3(lit_, "emissiveTint", Vector3{0, 0, 0});
  set3(lit_, "selfLight", Vector3{0, 0, 0});
  set3(lit_, "snowU", snow_u_);
  set3(lit_, "snowV", snow_v_);
  setF(lit_, "snowSeason", snow_tex_.id ? season_snow_ : 0.0f);
  setF(lit_, "cropStage", season_crop_);
  setF(lit_, "leafStage", season_leaf_);
  setF(lit_, "canopyCut", canopy_cut_);
  setI(lit_, "landOn", 0);
}

void Renderer::beginScene(const RenderOptions& o, const Lighting& L, const Camera3D& cam, float time_s) {
  opt_ = o;
  frame_L_ = L;
  draw_calls_ = 0;
  triangles_ = 0;
  ++frame_;
  post_active_ = o.post;
  if (post_active_) {
    ensureTargets();
    BeginTextureMode(scene_);
  }
  applyFrameUniforms(cam, L, time_s);
  bindGlobalTextures();
}

void Renderer::renderMirror(const Camera3D& rear, const Lighting& L, float time_s, const std::function<void()>& scene) {
  constexpr int kW = 512, kH = 144;
  if (!mirror_.id) {
    mirror_ = LoadRenderTexture(kW, kH);
    SetTextureFilter(mirror_.texture, TEXTURE_FILTER_BILINEAR);
  }
  frame_L_ = L;
  BeginTextureMode(mirror_);
  ClearBackground(BLACK);
  applyFrameUniforms(rear, L, time_s);
  bindGlobalTextures();
  sky_res_override_ = {static_cast<float>(kW), static_cast<float>(kH)};
  drawSky(rear, L, static_cast<float>(kW) / kH);
  sky_res_override_ = {0, 0};
  BeginMode3D(rear);
  scene();
  EndMode3D();
  EndTextureMode();
  mirror_ok_ = true;
}

void Renderer::drawMirrorGlass(const Vector3 c[4], float u0, float u1) {
  // A mirror shows the rear view flipped left-right; render-target rows run bottom-up.
  rlDrawRenderBatchActive();
  rlDisableColorBlend();
  rlColorMask(true, true, true, false);  // keep the scene's reflection mask (alpha) untouched
  rlSetTexture(mirror_.texture.id);
  rlBegin(RL_QUADS);
  rlColor4ub(235, 235, 235, 255);
  rlTexCoord2f(u1, 0.0f);
  rlVertex3f(c[0].x, c[0].y, c[0].z);
  rlTexCoord2f(u0, 0.0f);
  rlVertex3f(c[1].x, c[1].y, c[1].z);
  rlTexCoord2f(u0, 1.0f);
  rlVertex3f(c[2].x, c[2].y, c[2].z);
  rlTexCoord2f(u1, 1.0f);
  rlVertex3f(c[3].x, c[3].y, c[3].z);
  rlEnd();
  rlSetTexture(0);
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, true);
  rlEnableColorBlend();
}

void Renderer::setLights(const std::vector<PointLight>& lights) {
  const int n = static_cast<int>(std::min<size_t>(lights.size(), kMaxLights));
  setI(lit_, "numLights", n);
  for (int i = 0; i < n; ++i) {
    const Vector4 pr{lights[i].pos.x, lights[i].pos.y, lights[i].pos.z, lights[i].range};
    char name[32];
    std::snprintf(name, sizeof name, "lightPosR[%d]", i);
    SetShaderValue(lit_, GetShaderLocation(lit_, name), &pr, SHADER_UNIFORM_VEC4);
    std::snprintf(name, sizeof name, "lightCol[%d]", i);
    set3(lit_, name, lights[i].color);
  }
}

void Renderer::beginTransparent() {
  rlDrawRenderBatchActive();
  rlEnableColorBlend();
}

void Renderer::drawSky(const Camera3D& cam, const Lighting& L, float aspect) {
  const Matrix view = GetCameraMatrix(cam);
  const Matrix proj = MatrixPerspective(cam.fovy * DEG2RAD, aspect, 0.3, 4000.0);
  const Matrix inv = MatrixInvert(MatrixMultiply(view, proj));
  Vector2 res{static_cast<float>(post_active_ ? tw_ : GetRenderWidth()), static_cast<float>(post_active_ ? th_ : GetRenderHeight())};
  if (sky_res_override_.x > 0) res = sky_res_override_;
  set2(sky_, "resolution", res);
  SetShaderValueMatrix(sky_, GetShaderLocation(sky_, "invViewProj"), inv);
  set3(sky_, "camPos", cam.position);
  setF(sky_, "sunVisible", L.sun_visible);
  setF(sky_, "starBright", L.stars * 0.35f);
  set3(sky_, "cityGlow", Vector3Scale(Vector3{0.060f, 0.046f, 0.036f}, L.night * (1.0f - L.indoor)));
  bindGlobalTextures();
  rlDrawRenderBatchActive();
  rlDisableColorBlend();  // the sky writes alpha 0 (no reflection) and must still be opaque
  BeginShaderMode(sky_);
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
  EndShaderMode();
  // Opaque geometry follows: alpha carries the reflection amount for SSR, so no blending.
  rlDrawRenderBatchActive();
  rlDisableColorBlend();
}

void Renderer::drawMeshMat(const Mesh& m, const Matrix& model, int material, Color tint, Vector3 emissive) {
  setI(lit_, "materialOverride", material);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  set3(lit_, "emissiveTint", emissive);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = tint;
  DrawMesh(m, mat_, model);
  ++draw_calls_;
  triangles_ += m.triangleCount;
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "materialOverride", -1);
}

void Renderer::drawWorld(const Camera3D& cam, const World& world, bool photo_textures, bool neutral_floor) {
  (void)cam;
  bindGlobalTextures();
  const int loc_surface = GetShaderLocation(lit_, "surfaceMode");
  const int loc_tex = GetShaderLocation(lit_, "useTexture");
  auto mode = [&](int surface, int tex) {
    SetShaderValue(lit_, loc_surface, &surface, SHADER_UNIFORM_INT);
    SetShaderValue(lit_, loc_tex, &tex, SHADER_UNIFORM_INT);
  };
  rlDisableBackfaceCulling();
  // Terrain: ground raster classified into asphalt / paving / paint / greenery with detail maps.
  mode(1, 1);
  const int loc_land = GetShaderLocation(lit_, "landOn");
  for (const auto& [code, c] : world.cells()) {
    if (!c->gpu.terrain.vaoId) continue;
    rlActiveTextureSlot(kSlotAO);
    rlEnableTexture(c->gpu.detail.ao.id ? c->gpu.detail.ao.id : rlGetTextureIdDefault());
    rlActiveTextureSlot(kSlotLand);
    rlEnableTexture(c->gpu.detail.landcover.id ? c->gpu.detail.landcover.id : rlGetTextureIdDefault());
    rlActiveTextureSlot(0);
    const int land_on = c->gpu.detail.landcover.id ? 1 : 0;
    SetShaderValue(lit_, loc_land, &land_on, SHADER_UNIFORM_INT);
    mat_.maps[MATERIAL_MAP_DIFFUSE].texture = c->gpu.ground;
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    DrawMesh(c->gpu.terrain, mat_, c->model);
    ++draw_calls_;
    triangles_ += c->gpu.terrain.triangleCount;
  }
  {
    const int off = 0;
    SetShaderValue(lit_, loc_land, &off, SHADER_UNIFORM_INT);
  }
  rlActiveTextureSlot(kSlotLand);
  rlEnableTexture(rlGetTextureIdDefault());
  rlActiveTextureSlot(kSlotAO);
  rlEnableTexture(rlGetTextureIdDefault());
  rlActiveTextureSlot(0);
  // Photo-textured building faces (PLATEAU appearance atlases). With photo textures disabled
  // (they contain real signage / advertising) the same faces are drawn plain.
  if (photo_textures) mode(2, 1);
  else mode(0, 0);
  if (!photo_textures) mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{196, 194, 188, 255};
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.chunks.size(); ++i) {
      const int page = c->gpu.chunk_page[i];
      if (page < 0 || page >= static_cast<int>(c->gpu.pages.size()) || !c->gpu.pages[static_cast<size_t>(page)].id) continue;
      mat_.maps[MATERIAL_MAP_DIFFUSE].texture =
          photo_textures ? c->gpu.pages[static_cast<size_t>(page)] : Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
      DrawMesh(c->gpu.chunks[i], mat_, c->model);
      ++draw_calls_;
      triangles_ += c->gpu.chunks[i].triangleCount;
    }
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mode(0, 0);
  if (auto mz = world.minTerrainZ(); mz && neutral_floor) {
    // Neutral floor outside the data coverage (no invented content), below all real terrain.
    setI(lit_, "materialOverride", kMatConcrete);
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{92, 92, 90, 255};
    DrawMesh(plane_, mat_, MatrixTranslate(0.0f, static_cast<float>(*mz) - 1.5f, 0.0f));
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    setI(lit_, "materialOverride", -1);
  }
  // Procedural facades (fictional island): windows, glass, balconies, roofs, signs from style codes.
  mode(3, 0);
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.chunks.size(); ++i) {
      if (c->gpu.chunk_page[i] != kPageProcedural) continue;
      DrawMesh(c->gpu.chunks[i], mat_, c->model);
      ++draw_calls_;
      triangles_ += c->gpu.chunks[i].triangleCount;
    }
  mode(0, 0);
  // Vertex-coloured buildings (LOD1 / untextured LOD2, rooftop equipment).
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.chunks.size(); ++i) {
      const int page = c->gpu.chunk_page[i];
      if (page == kPageProcedural) continue;
      if (page >= 0 && page < static_cast<int>(c->gpu.pages.size()) && c->gpu.pages[static_cast<size_t>(page)].id) continue;
      setI(lit_, "materialOverride", kMatWallPaint);
      DrawMesh(c->gpu.chunks[i], mat_, c->model);
      ++draw_calls_;
      triangles_ += c->gpu.chunks[i].triangleCount;
    }
  setI(lit_, "materialOverride", -1);
  // Street detail: sidewalks, curbs, markings, furniture (materials per vertex; see-through glass
  // is left for drawClearGlass).
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.detail.meshes.size(); ++i) {
      if (c->gpu.detail.mats[i] == kMatClearGlass) continue;
      const Mesh& m = c->gpu.detail.meshes[i];
      DrawMesh(m, mat_, c->model);
      ++draw_calls_;
      triangles_ += m.triangleCount;
    }
  // Real PLATEAU trees (procedural shape) and planting.
  for (const auto& [code, c] : world.cells()) {
    for (const auto& t : c->gpu.detail.trees) {
      const Matrix m = MatrixMultiply(MatrixTranslate(t.base[0], t.base[1], t.base[2]), c->model);
      if (t.bark.vaoId) {
        mode(0, 0);
        DrawMesh(t.bark, mat_, m);
      }
      if (t.leaves.vaoId) {
        mode(0, 1);
        mat_.maps[MATERIAL_MAP_DIFFUSE].texture = leaf_tex_;
        DrawMesh(t.leaves, mat_, m);
        mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
        triangles_ += t.leaves.triangleCount;
      }
      draw_calls_ += 2;
    }
    if (c->gpu.detail.hedge.vaoId) {
      mode(0, 1);
      mat_.maps[MATERIAL_MAP_DIFFUSE].texture = leaf_tex_;
      DrawMesh(c->gpu.detail.hedge, mat_, c->model);
      mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
    }
  }
  mode(0, 0);
  rlEnableBackfaceCulling();
}

void Renderer::drawPlayerBody(const Vector3& feet, float yaw_rad) {
  setI(lit_, "materialOverride", -1);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  rlDisableBackfaceCulling();
  const Matrix M = MatrixMultiply(MatrixRotateY(-yaw_rad), MatrixTranslate(feet.x, feet.y, feet.z));
  drawHuman(humans_.frame(BodyVariant::Trousers, 0.0f, true), M, Color{40, 60, 110, 255}, Color{30, 32, 38, 255}, Color{222, 186, 150, 255},
            Color{24, 20, 18, 255});
  rlEnableBackfaceCulling();
}

void Renderer::drawPlayerUmbrella(const Vector3& feet, float yaw_rad, bool first_person) {
  humans_.build();
  setI(lit_, "materialOverride", -1);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  rlDisableBackfaceCulling();
  // (first person the shaft would run past the eye: the hand holds it forward and to the right)
  const Matrix off = first_person ? MatrixTranslate(0.16f, 0.0f, -0.22f) : MatrixIdentity();
  const Matrix M = MatrixMultiply(MatrixMultiply(off, MatrixRotateY(-yaw_rad)), MatrixTranslate(feet.x, feet.y, feet.z));
  set3(lit_, "partTop", Vector3{0.88f, 0.9f, 0.92f});  // a clear vinyl one, as most in Tokyo
  DrawMesh(humans_.umbrella(), mat_, M);
  ++draw_calls_;
  rlEnableBackfaceCulling();
}

void Renderer::drawStandingPerson(const Vector3& feet, float yaw_rad, int variant, Color shirt, Color pants) {
  humans_.build();
  setI(lit_, "materialOverride", -1);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  rlDisableBackfaceCulling();
  const Matrix M = MatrixMultiply(MatrixRotateY(-yaw_rad), MatrixTranslate(feet.x, feet.y, feet.z));
  drawHuman(humans_.frame(static_cast<BodyVariant>(variant % static_cast<int>(BodyVariant::Count)), 0.0f, true), M, shirt, pants,
            Color{222, 186, 150, 255}, Color{28, 22, 20, 255});
  rlEnableBackfaceCulling();
}

void Renderer::drawInterior(const Interior& in) {
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", 0);
  rlDisableBackfaceCulling();
  for (const auto& m : in.meshes()) {
    DrawMesh(m, mat_, in.model());
    ++draw_calls_;
  }
  rlEnableBackfaceCulling();
  setI(lit_, "materialOverride", -1);
}

void Renderer::drawFacades(const FacadeDetail& f) {
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  rlDisableBackfaceCulling();
  const Matrix id = MatrixIdentity();
  f.forEachMesh([&](const Mesh& m, int) {
    DrawMesh(m, mat_, id);
    ++draw_calls_;
    triangles_ += m.triangleCount;
  });
  rlEnableBackfaceCulling();
}

void Renderer::drawMarkings(const RoadMarkings& rm) {
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  rlDisableBackfaceCulling();
  const Matrix id = MatrixIdentity();
  rm.forEachMesh([&](const Mesh& m) {
    DrawMesh(m, mat_, id);
    ++draw_calls_;
    triangles_ += m.triangleCount;
  });
  rlEnableBackfaceCulling();
}

void Renderer::drawClearGlass(const World& world) {
  // after everything opaque: blended over what lies behind, without writing depth; the scene's
  // reflection mask (alpha) is kept, the glass's own reflection is in its colour (lit shader, id 40)
  rlDrawRenderBatchActive();
  rlEnableColorBlend();
  rlSetBlendMode(BLEND_ALPHA);
  rlColorMask(true, true, true, false);
  rlDisableDepthMask();
  rlDisableBackfaceCulling();
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.detail.meshes.size(); ++i)
      if (c->gpu.detail.mats[i] == kMatClearGlass) {
        DrawMesh(c->gpu.detail.meshes[i], mat_, c->model);
        ++draw_calls_;
      }
  rlDrawRenderBatchActive();
  rlEnableBackfaceCulling();
  rlEnableDepthMask();
  rlColorMask(true, true, true, true);
  rlDisableColorBlend();
}

void Renderer::drawOcean(const Camera3D& cam, float sea_y) {
  rlDrawRenderBatchActive();
  rlDisableColorBlend();  // opaque: alpha carries the reflection amount
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", kMatWater);
  const float gx = std::round(cam.position.x / 500.0f) * 500.0f, gz = std::round(cam.position.z / 500.0f) * 500.0f;
  DrawMesh(ocean_, mat_, MatrixTranslate(gx, sea_y, gz));
  ++draw_calls_;
  setI(lit_, "materialOverride", -1);
}

void Renderer::drawCellSeas(const World& world) {
  rlDrawRenderBatchActive();
  rlDisableColorBlend();  // opaque: alpha carries the reflection amount
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", kMatWater);
  rlDisableBackfaceCulling();
  for (const auto& [code, c] : world.cells())
    if (c->gpu.sea.vaoId) {
      DrawMesh(c->gpu.sea, mat_, c->model);
      ++draw_calls_;
    }
  rlEnableBackfaceCulling();
  setI(lit_, "materialOverride", -1);
}

void Renderer::drawFarView(const FarView& far, const World& world, const Camera3D& cam, float sea_y, float fog_density) {
  if (!far.ready() || !world.hasOrigin()) return;
  bindGlobalTextures();
  setF(lit_, "fogDensity", fog_density);
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  const Vector3 fr = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
  const rj::geo::Vec3d fw = rlToEnu(fr);
  rlDisableBackfaceCulling();
  // ocean to the horizon (the near pass draws its own around the camera)
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", kMatWater);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  {
    // (centred on the camera: the curvature is relative to the point below it; the waves are in
    // world coordinates, so the moving mesh does not drag them along)
    DrawMesh(ocean_curved_, mat_, MatrixTranslate(cam.position.x, sea_y - 0.15f, cam.position.z));
    ++draw_calls_;
  }
  setI(lit_, "materialOverride", -1);
  const auto& O = world.origin().frame();
  // terrain tiles standing in for cells that are not loaded
  setI(lit_, "surfaceMode", 4);
  setI(lit_, "useTexture", 1);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = far.colorMap();
  std::vector<std::pair<const FarView::Tile*, Matrix>> near_boxes;
  for (const auto& t : far.tiles()) {
    if (world.cells().count(t.mesh)) continue;
    const rj::geo::Rigid3d X = O.transformFrom(t.frame);
    const rj::geo::Vec3d c = X.apply({0.0, 0.0, t.zmax * 0.5});
    const rj::geo::Vec3d d{c.x - cp.x, c.y - cp.y, c.z - cp.z};
    const double dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist > 85000.0) continue;
    if (d.x * fw.x + d.y * fw.y + d.z * fw.z < -t.radius - t.zmax) continue;  // behind the camera
    const Matrix M = rigidToRaylib(X);
    if (t.terrain.vaoId) {
      DrawMesh(t.terrain, mat_, M);
      ++draw_calls_;
      triangles_ += t.terrain.triangleCount;
    }
    if (!t.boxes.empty() && dist < 30000.0) near_boxes.push_back({&t, M});
  }
  // building boxes (vertex colours, material from the vertices)
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  for (const auto& [t, M] : near_boxes)
    for (const auto& m : t->boxes) {
      DrawMesh(m, mat_, M);
      ++draw_calls_;
      triangles_ += m.triangleCount;
    }
  rlEnableBackfaceCulling();
  setF(lit_, "fogDensity", frame_L_.fog_density);
}

void Renderer::drawShips(const Ferries& ferries, const Camera3D& cam, const Lighting& L) {
  if (!ferries.loaded()) return;
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  rlDisableBackfaceCulling();
  for (const auto& f : ferries.ships()) {
    if (f.phase == Ferry::Phase::Offmap || std::hypot(f.pos.x - c.x, f.pos.y - c.y) > 4500.0) continue;
    const Vector3 p = enuToRl({f.pos.x, f.pos.y, f.pos.z + f.heave});
    const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-f.roll), MatrixRotateX(f.pitch)), MatrixRotateY(-f.yaw)),
                                    MatrixTranslate(p.x, p.y, p.z));
    const Mesh& m = ship_models_.get(f.cls).hull;
    DrawMesh(m, mat_, M);
    ++draw_calls_;
    triangles_ += m.triangleCount;
  }
  rlEnableBackfaceCulling();
  // Wakes: foam trail from the stern, widening and fading with age, plus the bow wave.
  // (Unlit foam, dimmed with the daylight.)
  const float day = std::clamp(0.12f + 0.9f * (1.0f - L.night) * std::min(1.0f, (L.sun_color.x + L.ambient_sky.x) * 0.8f), 0.08f, 1.0f);
  const unsigned char fc = static_cast<unsigned char>(235 * day);
  rlDrawRenderBatchActive();
  rlDisableDepthMask();
  rlDisableBackfaceCulling();
  rlColorMask(true, true, true, false);  // keep the reflection mask in alpha
  rlSetTexture(rlGetTextureIdDefault());
  for (const auto& f : ferries.ships()) {
    if (f.phase == Ferry::Phase::Offmap || f.trail.size() < 2 || f.wake < 0.05f) continue;
    if (std::hypot(f.pos.x - c.x, f.pos.y - c.y) > 3000.0) continue;
    const ShipClass& C = Ferries::shipClass(f.cls);
    const size_t n = f.trail.size();
    rlBegin(RL_QUADS);
    for (size_t i = n - 1; i > 0; --i) {
      const auto& a = f.trail[i];
      const auto& b = f.trail[i - 1];
      const double dx = a.x - b.x, dy = a.y - b.y, l = std::max(1e-3, std::hypot(dx, dy));
      const double nx = -dy / l, ny = dx / l;
      const float ageA = static_cast<float>(n - 1 - i) / static_cast<float>(n), ageB = static_cast<float>(n - i) / static_cast<float>(n);
      const float wA = C.beam * (0.45f + 1.8f * ageA), wB = C.beam * (0.45f + 1.8f * ageB);
      const unsigned char aA = static_cast<unsigned char>(170 * f.wake * (1.0f - ageA)), aB = static_cast<unsigned char>(170 * f.wake * (1.0f - ageB));
      const Vector3 A0 = enuToRl({a.x + nx * wA, a.y + ny * wA, a.z + 0.06}), A1 = enuToRl({a.x - nx * wA, a.y - ny * wA, a.z + 0.06});
      const Vector3 B0 = enuToRl({b.x + nx * wB, b.y + ny * wB, b.z + 0.06}), B1 = enuToRl({b.x - nx * wB, b.y - ny * wB, b.z + 0.06});
      rlColor4ub(fc, fc, fc, aA);
      rlVertex3f(A0.x, A0.y, A0.z);
      rlVertex3f(A1.x, A1.y, A1.z);
      rlColor4ub(fc, fc, fc, aB);
      rlVertex3f(B1.x, B1.y, B1.z);
      rlVertex3f(B0.x, B0.y, B0.z);
    }
    // bow wave: two short foam wedges along the hull
    const double fx = std::sin(f.yaw), fy = std::cos(f.yaw), rx = fy, ry = -fx;
    for (float sg : {1.0f, -1.0f}) {
      const rj::geo::Vec3d bow{f.pos.x + fx * C.length * 0.48, f.pos.y + fy * C.length * 0.48, f.pos.z + 0.07};
      const rj::geo::Vec3d mid{f.pos.x + fx * C.length * 0.1 + rx * sg * C.beam * 0.62, f.pos.y + fy * C.length * 0.1 + ry * sg * C.beam * 0.62, f.pos.z + 0.07};
      const rj::geo::Vec3d out{mid.x + rx * sg * 3.0 - fx * 10.0, mid.y + ry * sg * 3.0 - fy * 10.0, mid.z};
      const Vector3 P0 = enuToRl(bow), P1 = enuToRl(mid), P2 = enuToRl(out);
      rlColor4ub(fc, fc, fc, static_cast<unsigned char>(200 * f.wake));
      rlVertex3f(P0.x, P0.y, P0.z);
      rlVertex3f(P1.x, P1.y, P1.z);
      rlColor4ub(fc, fc, fc, 0);
      rlVertex3f(P2.x, P2.y, P2.z);
      rlVertex3f(P2.x, P2.y, P2.z);
    }
    rlEnd();
  }
  rlSetTexture(0);
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, true);
  rlEnableDepthMask();
  rlEnableBackfaceCulling();
}

void Renderer::drawAircraft(const Aviation& av, const Camera3D& cam, const Lighting& L, int ride_jet, const FlightView* fv, bool jet_outside) {
  if (!av.loaded()) return;
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  rlDisableBackfaceCulling();
  const JetModel& J = aircraft_models_.jet();
  const float nav = 3.0f + 9.0f * L.night;
  const bool strobe = std::fmod(GetTime(), 1.3) < 0.08;
  for (const auto& a : av.airliners()) {
    if (a.phase == Airliner::Phase::Offmap || std::hypot(a.pos.x - c.x, a.pos.y - c.y) > 9000.0) continue;
    const Vector3 p = enuToRl(a.pos);
    const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a.roll), MatrixRotateX(a.pitch)), MatrixRotateY(-a.yaw)),
                                    MatrixTranslate(p.x, p.y, p.z));
    const bool at_stand = a.phase == Airliner::Phase::AtStand;
    const double dist = std::hypot(a.pos.x - c.x, a.pos.y - c.y);
    if (a.id == ride_jet || (at_stand && dist < 70.0)) {  // inside, or seen through the open door
      set3(lit_, "selfLight", kJetCabinLight);
      drawMeshMat(J.cabin, M, -1, WHITE);
      set3(lit_, "selfLight", Vector3{0, 0, 0});
    }
    if (a.id != ride_jet || jet_outside) drawMeshMat(J.fuselage, M, -1, WHITE);
    if (!at_stand) drawMeshMat(J.door, M, -1, WHITE);
    if (at_stand && dist < 400.0) drawMeshMat(J.stairs, M, -1, WHITE);
    drawMeshMat(J.wings, M, -1, WHITE);
    if (a.gear > 0.3f) drawMeshMat(J.gear, M, -1, WHITE);
    drawMeshMat(J.nav_red, M, kMatSignalLamp, WHITE, Vector3{nav, nav * 0.04f, nav * 0.03f});
    drawMeshMat(J.nav_green, M, kMatSignalLamp, WHITE, Vector3{nav * 0.05f, nav, nav * 0.25f});
    drawMeshMat(J.nav_white, M, kMatSignalLamp, WHITE, strobe ? Vector3{30, 30, 30} : Vector3{0.3f, 0.3f, 0.3f});
  }
  // light aircraft
  const LightPlane& pl = av.plane();
  const LightPlaneModel& Lm = aircraft_models_.light();
  if (!pl.crashed() || fv) {
    const Matrix M = pl.modelMatrix();
    const bool pit = fv && fv->cockpit;
    drawMeshMat(pit ? Lm.cockpit : Lm.fuselage, M, -1, WHITE);
    drawMeshMat(Lm.rest, M, -1, WHITE);
    const Matrix P = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-pl.prop()), MatrixTranslate(0, -0.1f, -Lm.prop_y)), M);
    drawMeshMat(Lm.prop, P, -1, WHITE);
    if (pit) {
      const float glow = 0.6f + 1.2f * L.night;
      drawMeshMat(Lm.dial_marks, M, -1, WHITE, Vector3{glow, glow, glow});
      auto needle = [&](int dial, float deg, float scale, float dz, Vector3 col) {
        const float* d = Lm.dial[dial];
        const Matrix N = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(1, scale, 1), MatrixRotateZ(-deg * DEG2RAD)),
                                                       MatrixTranslate(d[0], d[2] + dz, -d[1])), M);
        drawMeshMat(Lm.needle, N, -1, WHITE, Vector3Scale(col, glow * 1.6f));
      };
      const Vector3 W{1, 1, 1}, O{1.0f, 0.45f, 0.08f};
      needle(0, -150.0f + 300.0f * std::clamp(fv->kt / 200.0f, 0.0f, 1.0f), 1.0f, 0, W);
      const float off = std::clamp(-fv->pitch * 0.0012f, -0.035f, 0.035f);
      needle(1, 90.0f - fv->roll, 1.0f, off, W);  // horizon line
      needle(1, -90.0f - fv->roll, 1.0f, off, W);
      needle(1, 90.0f, 0.45f, 0, O);               // fixed aeroplane symbol
      needle(1, -90.0f, 0.45f, 0, O);
      needle(2, 360.0f * std::fmod(std::max(0.0f, fv->alt_ft), 1000.0f) / 1000.0f, 1.0f, 0, W);
      needle(2, 360.0f * std::max(0.0f, fv->alt_ft) / 10000.0f, 0.6f, 0, W);
      const float turn = std::clamp(fv->turn_dps / 3.0f, -1.0f, 1.0f) * 25.0f;
      needle(3, 90.0f + turn, 0.8f, 0, W);
      needle(3, -90.0f + turn, 0.8f, 0, W);
      needle(4, fv->heading, 1.0f, 0, O);
      needle(5, -90.0f + std::clamp(fv->vs_fpm / 2000.0f, -1.0f, 1.0f) * 170.0f, 1.0f, 0, W);
      const float* y = Lm.yoke_pos;
      const Matrix Y = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-fv->aileron * 0.6f), MatrixTranslate(y[0], y[2], -(y[1] - fv->elevator * 0.07f))), M);
      drawMeshMat(Lm.yoke, Y, -1, WHITE);
    }
  }
  rlEnableBackfaceCulling();
}

void Renderer::drawTrains(const Trains& trains, const Camera3D& cam, int ride_train, int ride_car) {
  if (!trains.loaded()) return;
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  setI(lit_, "surfaceMode", 0);
  setI(lit_, "useTexture", 0);
  setI(lit_, "materialOverride", -1);
  set3(lit_, "emissiveTint", Vector3{2.5f, 2.4f, 2.2f});
  rlDisableBackfaceCulling();
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  for (const auto& t : trains.trains()) {
    const bool shink = trains.lines()[static_cast<size_t>(t.line)].kind == LineKind::Shinkansen;
    for (int k = 0; k < t.cars; ++k) {
      rj::geo::Vec3d p;
      float yaw, pitch;
      trains.carPose(t, k, p, yaw, pitch);
      if (std::hypot(p.x - c.x, p.y - c.y) > 1100.0) continue;
      const bool end = k == 0 || k == t.cars - 1;
      const bool reversed = k == t.cars - 1 && k > 0;  // the rear cab faces backwards
      const TrainCarModel& m = train_models_.get(shink ? (end ? TrainCar::ShinkansenNose : (k % 4 == 2 ? TrainCar::ShinkansenPanto : TrainCar::ShinkansenMid))
                                                       : (end ? TrainCar::CommuterCab : (k % 3 == 1 ? TrainCar::CommuterPanto : TrainCar::CommuterMid)));
      const float y = reversed ? yaw + PI : yaw;
      const float pt = reversed ? -pitch : pitch;
      const Vector3 rp = enuToRl(p);
      const Matrix M = MatrixMultiply(MatrixMultiply(MatrixRotateX(pt), MatrixRotateY(-y)), MatrixTranslate(rp.x, rp.y, rp.z));
      DrawMesh(m.shell, mat_, M);
      const bool riding = t.id == ride_train && k == ride_car;
      if (!riding && m.glass.vaoId) DrawMesh(m.glass, mat_, M);
      // doors: the platform is on the left of the direction of travel (the rear cab car is
      // turned round, so its left is the model's right); leaves slide along the car (model y =
      // raylib -z)
      const float open = Trains::doorOpen(t);
      const int open_side = reversed ? 1 : 0;
      for (int side = 0; side < 2; ++side)
        for (int dir = 0; dir < 2; ++dir) {
          const Mesh& dm = m.doors[side][dir];
          if (!dm.vaoId) continue;
          const float d = side == open_side ? open * m.door_travel * (dir ? 1.0f : -1.0f) : 0.0f;
          DrawMesh(dm, mat_, d != 0.0f ? MatrixMultiply(MatrixTranslate(0, 0, -d), M) : M);
        }
      // the lit interior of the car ridden, and of nearby cars standing with their doors open
      const bool near_open = open > 0.0f && std::hypot(p.x - c.x, p.y - c.y) < 45.0;
      const bool next_car = t.id == ride_train && std::abs(k - ride_car) == 1;  // (seen through the gangway)
      if ((riding || near_open || next_car) && m.interior.vaoId) {
        set3(lit_, "selfLight", kCabinLight);
        DrawMesh(m.interior, mat_, M);
        set3(lit_, "selfLight", Vector3{0, 0, 0});
      }
      draw_calls_ += 2;
      triangles_ += m.shell.triangleCount;
    }
  }
  rlEnableBackfaceCulling();
}

void Renderer::drawBox(const rj::geo::Vec3d& c, float yaw, Vector3 half, int material, Color tint, Vector3 emissive, float pitch) {
  const Vector3 p = enuToRl(c);
  const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(half.x * 2.0f, half.z * 2.0f, half.y * 2.0f), MatrixRotateX(pitch)), MatrixRotateY(-yaw)),
                                  MatrixTranslate(p.x, p.y, p.z));
  drawMeshMat(unit_box_, M, material, tint, emissive);
}

void Renderer::drawGates(const Trains& trains, const Camera3D& cam, int shut_gate, int shut_lane, int flash_gate, int flash_lane, bool flash_ok) {
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  const auto& gates = trains.gates();
  auto at = [&](const StationGate& G, double u, double v, double z) {
    const double th = G.heading * DEG2RAD;
    return rj::geo::Vec3d{G.pos.x + std::sin(th) * u + std::cos(th) * v, G.pos.y + std::cos(th) * u - std::sin(th) * v, G.pos.z + z};
  };
  if (shut_gate >= 0 && shut_gate < static_cast<int>(gates.size())) {
    const StationGate& G = gates[static_cast<size_t>(shut_gate)];
    if (std::hypot(G.pos.x - cp.x, G.pos.y - cp.y) < 80.0 && shut_lane >= 0 && shut_lane < static_cast<int>(G.lanes.size())) {
      const float yaw = static_cast<float>(G.heading * DEG2RAD);
      const double v = G.lanes[static_cast<size_t>(shut_lane)];
      for (double s : {-1.0, 1.0})  // the two flaps, out from the cabinets across the lane
        drawBox(at(G, 0.0, v + s * 0.26, 0.78), yaw, {0.25f, 0.018f, 0.16f}, kMatDefault, Color{200, 40, 50, 255}, Vector3{0.25f, 0.02f, 0.02f});
    }
  }
  if (flash_gate >= 0 && flash_gate < static_cast<int>(gates.size())) {
    const StationGate& G = gates[static_cast<size_t>(flash_gate)];
    if (flash_lane >= 0 && flash_lane < static_cast<int>(G.lanes.size())) {
      const float yaw = static_cast<float>(G.heading * DEG2RAD);
      const double v = G.lanes[static_cast<size_t>(flash_lane)] - 0.5;  // the reader on the cabinet at the lane's left
      const Vector3 e = flash_ok ? Vector3{0.2f, 2.2f, 0.6f} : Vector3{2.4f, 0.15f, 0.1f};
      for (double u : {-0.55, 0.55})
        drawBox(at(G, u, v, 1.07), yaw, {0.07f, 0.1f, 0.012f}, kMatSignalLamp, WHITE, e);
    }
  }
}

void Renderer::drawCrossings(const Trains& trains, const Camera3D& cam, float time_s) {
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  const bool odd = std::fmod(time_s, 1.0f) < 0.5f;  // the two lamps flash in turn, about once a second each
  for (const auto& c : trains.crossings()) {
    if (std::hypot(c.pos.x - cp.x, c.pos.y - cp.y) > 400.0) continue;
    for (const auto& st : c.sets) {
      // arm: yellow and black stripes from the pivot on the barrier machine (1.0 m up); raised it
      // stands nearly upright, lowered it lies across the lanes
      const double hd = st.arm_hd * DEG2RAD;
      const double up = (1.0 - c.arm) * 82.0 * DEG2RAD;
      const rj::geo::Vec3d piv{st.pos.x, st.pos.y, st.pos.z + 1.0};
      const double fx = std::sin(hd) * std::cos(up), fy = std::cos(hd) * std::cos(up), fz = std::sin(up);
      const int n = std::max(4, static_cast<int>(st.arm_len / 0.5));
      const double seg = st.arm_len / n;
      for (int k = 0; k < n; ++k) {
        const double m = (k + 0.5) * seg;
        const rj::geo::Vec3d cc{piv.x + fx * m, piv.y + fy * m, piv.z + fz * m};
        drawBox(cc, static_cast<float>(hd), {0.045f, static_cast<float>(seg * 0.5), 0.045f}, kMatDefault,
                k % 2 == 0 ? Color{240, 196, 20, 255} : Color{24, 24, 24, 255}, {0, 0, 0}, static_cast<float>(up));
      }
      // warning lamps (red), facing the approaching traffic, on the post 2.45 m up
      const double f = st.facing * DEG2RAD;
      const double fvx = std::sin(f), fvy = std::cos(f), svx = std::cos(f), svy = -std::sin(f);
      for (int side = 0; side < 2; ++side) {
        const double sd = side ? 0.36 : -0.36;
        const rj::geo::Vec3d lp{st.pos.x + fvx * 0.23 + svx * sd, st.pos.y + fvy * 0.23 + svy * sd, st.pos.z + 2.45};
        const bool lit = c.warning && (side == 0) == odd;
        drawBox(lp, static_cast<float>(f), {0.13f, 0.02f, 0.13f}, kMatSignalLamp, WHITE, lit ? Vector3{9.0f, 0.25f, 0.1f} : Vector3{0.05f, 0.0f, 0.0f});
      }
      // the direction indicator (an arrow lamp under the lamps) lit while warning
      const rj::geo::Vec3d ip{st.pos.x + fvx * 0.14, st.pos.y + fvy * 0.14, st.pos.z + 3.05};
      drawBox(ip, static_cast<float>(f), {0.12f, 0.01f, 0.06f}, kMatSignalLamp, WHITE, c.warning ? Vector3{4.0f, 1.6f, 0.2f} : Vector3{0.02f, 0.01f, 0.0f});
    }
  }
}

void Renderer::drawDistantTraffic(const Traffic& traffic, const Camera3D& cam, const Lighting& L, float time_s, int hour) {
  if (!traffic.loaded()) return;
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  const bool night = L.night > 0.35f;
  const double r0 = 330.0, r1 = night ? 2600.0 : 950.0;
  // cars per km per direction on a main road by the hour (a game assumption shaped like a weekday)
  static const float kPerKm[24] = {3, 2, 1.5f, 1.5f, 2, 5, 12, 22, 24, 18, 16, 16, 16, 16, 16, 17, 19, 23, 22, 16, 12, 9, 7, 5};
  const float base = kPerKm[std::clamp(hour, 0, 23)];
  rlDrawRenderBatchActive();
  rlDisableBackfaceCulling();
  rlColorMask(true, true, true, false);  // (no reflections: keep the scene's mask)
  rlBegin(RL_QUADS);
  const auto& E = traffic.edges();
  for (size_t ei = 0; ei < E.size(); ++ei) {
    const auto& e = E[ei];
    if (e.pts.size() < 2 || e.width < 5.5f) continue;  // (lanes and alleys: nobody far off)
    const auto& m = e.pts[e.pts.size() / 2];
    const double dm = std::hypot(m.x - cp.x, m.y - cp.y);
    if (dm < r0 - e.length || dm > r1 + e.length) continue;
    const float per_km = base * std::clamp((e.width - 4.0f) / 6.0f, 0.3f, 2.0f);
    const int n = static_cast<int>(e.length / 1000.0 * per_km + (static_cast<float>((ei * 2654435761u) % 1000u) / 1000.0f));
    for (int dir = 0; dir < 2; ++dir)
      for (int k = 0; k < n; ++k) {
        const uint32_t h = static_cast<uint32_t>(ei * 7919u + static_cast<uint32_t>(k) * 104729u + static_cast<uint32_t>(dir) * 15485863u);
        const double v = e.v0 * (0.75 + 0.3 * ((h >> 8) & 255u) / 255.0);
        double s = std::fmod(((h & 0xffffu) / 65535.0) * e.length + time_s * v, e.length);
        if (dir) s = e.length - s;
        // position on the polyline, kept to the left of the direction of travel
        size_t j = 1;
        while (j + 1 < e.cum.size() && e.cum[j] < s) ++j;
        const auto& A = e.pts[j - 1];
        const auto& B = e.pts[j];
        const double seg = std::max(1e-3, e.cum[j] - e.cum[j - 1]), t = std::clamp((s - e.cum[j - 1]) / seg, 0.0, 1.0);
        double fx = (B.x - A.x) / seg, fy = (B.y - A.y) / seg;
        if (dir) fx = -fx, fy = -fy;
        const double lx = -fy, ly = fx, off = e.lane_w * 0.5;
        const rj::geo::Vec3d p{A.x + (B.x - A.x) * t + lx * off, A.y + (B.y - A.y) * t + ly * off, A.z + (B.z - A.z) * t};
        const double d = std::hypot(p.x - cp.x, p.y - cp.y);
        if (d < r0 || d > r1) continue;
        auto quad = [&](double ax, double ay, double az, double hw, double hh, Color c) {  // a small square facing the camera
          const double vx = cp.x - ax, vy = cp.y - ay, vl = std::max(1e-3, std::hypot(vx, vy));
          const double sx = -vy / vl * hw, sy = vx / vl * hw;
          const Vector3 q0 = enuToRl({ax - sx, ay - sy, az - hh}), q1 = enuToRl({ax + sx, ay + sy, az - hh});
          const Vector3 q2 = enuToRl({ax + sx, ay + sy, az + hh}), q3 = enuToRl({ax - sx, ay - sy, az + hh});
          rlColor4ub(c.r, c.g, c.b, c.a);
          rlVertex3f(q0.x, q0.y, q0.z);
          rlVertex3f(q1.x, q1.y, q1.z);
          rlVertex3f(q2.x, q2.y, q2.z);
          rlVertex3f(q3.x, q3.y, q3.z);
        };
        if (night) {  // lamps, a little larger far off so they stay visible
          const double sz = 0.25 + d * 0.0006;
          quad(p.x + fx * 2.0, p.y + fy * 2.0, p.z + 0.8, sz, sz, Color{255, 246, 220, 255});
          quad(p.x - fx * 2.0, p.y - fy * 2.0, p.z + 0.9, sz * 0.8, sz * 0.8, Color{255, 40, 30, 255});
        } else {
          static const Color kBody[6] = {{200, 200, 204, 255}, {40, 40, 44, 255}, {150, 152, 156, 255}, {230, 230, 226, 255}, {70, 80, 110, 255}, {120, 40, 40, 255}};
          const Color c = kBody[(h >> 20) % 6];
          const float fog = static_cast<float>(std::clamp((d - r0) / (r1 - r0), 0.0, 1.0)) * 0.6f;  // (fades into the haze)
          const Color cf{static_cast<unsigned char>(c.r + (170 - c.r) * fog), static_cast<unsigned char>(c.g + (178 - c.g) * fog),
                         static_cast<unsigned char>(c.b + (190 - c.b) * fog), 255};
          quad(p.x, p.y, p.z + 0.75, 1.2, 0.72, cf);
        }
      }
  }
  rlEnd();
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, true);
  rlEnableBackfaceCulling();
}

void Renderer::drawSignals(const TrafficSignals& ts, const Camera3D& cam) {
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  const Vector3 kGreen{0.0f, 0.95f, 0.62f}, kYellow{1.0f, 0.62f, 0.0f}, kRed{1.0f, 0.07f, 0.03f};
  auto lamp = [&](const Mesh& m, const rj::geo::Vec3d& p, float facing, bool on, Vector3 col, float intensity) {
    // Mesh axis (+Y) -> facing direction (horizontal), see coords: ENU (sin f, cos f) -> raylib (sin f, 0, -cos f).
    const Matrix r = MatrixMultiply(MatrixRotateX(PI / 2), MatrixRotateY(PI - facing));
    const Vector3 rp = enuToRl(p);
    const Vector3 e = on ? Vector3Scale(col, intensity) : Vector3Scale(col, 0.015f);
    drawMeshMat(m, MatrixMultiply(r, MatrixTranslate(rp.x, rp.y, rp.z)), kMatSignalLamp, WHITE, e);
  };
  for (const auto& h : ts.heads()) {
    if (std::hypot(h.pos.x - c.x, h.pos.y - c.y) > 220.0) continue;
    const double fx = std::sin(h.facing), fy = std::cos(h.facing);
    // Viewer looks along -facing; the viewer's right is (-cos f, sin f).
    const double rx = -fy, ry = fx;
    if (h.kind == 0) {
      const VehLamp st = ts.vehicle(h.group, h.phase);
      const double sp = std::clamp(h.length / 3.0, 0.3, 0.45);
      const rj::geo::Vec3d base{h.pos.x + fx * 0.17, h.pos.y + fy * 0.17, h.pos.z};
      lamp(lens_, {base.x - rx * sp, base.y - ry * sp, base.z}, h.facing, st == VehLamp::Green, kGreen, 9.0f);
      lamp(lens_, base, h.facing, st == VehLamp::Yellow, kYellow, 9.0f);
      lamp(lens_, {base.x + rx * sp, base.y + ry * sp, base.z}, h.facing, st == VehLamp::Red, kRed, 9.0f);
    } else {
      const PedLamp st = ts.pedestrian(h.group, h.phase);
      const rj::geo::Vec3d base{h.pos.x + fx * 0.14, h.pos.y + fy * 0.14, h.pos.z};
      const bool walk = st == PedLamp::Walk || (st == PedLamp::Flash && ts.flashOn());
      lamp(pedlens_, {base.x, base.y, base.z + 0.16}, h.facing, st == PedLamp::Stop, kRed, 7.0f);
      lamp(pedlens_, {base.x, base.y, base.z - 0.16}, h.facing, walk, kGreen, 7.0f);
    }
  }
}

namespace {
Matrix vehicleMatrix(const Vehicle& v, bool body = true) {
  const Vector3 p = enuToRl(v.pos);
  const Matrix tilt = body ? MatrixMultiply(MatrixRotateZ(-v.roll), MatrixRotateX(v.pitch)) : MatrixRotateX(v.pitch);
  return MatrixMultiply(MatrixMultiply(tilt, MatrixRotateY(-v.yaw)), MatrixTranslate(p.x, p.y, p.z));
}
Vector3 modelToRl(const float* p) { return {p[0], p[2], -p[1]}; }
}  // namespace

void Renderer::vehicleCasters(const Traffic& traffic, const Camera3D& cam, std::vector<Caster>& out, const Vehicle* extra) const {
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  for (const auto& v : traffic.vehicles())
    if (std::hypot(v.pos.x - c.x, v.pos.y - c.y) < 90.0) out.push_back({&vehicles_.get(v.type).body, vehicleMatrix(v)});
  if (extra) out.push_back({&vehicles_.get(extra->type).body, vehicleMatrix(*extra)});
}

void Renderer::drawVehicles(const Traffic& traffic, const Camera3D& cam, const Lighting& L, const Vehicle* extra, const CockpitView* cockpit,
                            bool extra_driven) {
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  rlDisableBackfaceCulling();
  std::vector<const Vehicle*> list;
  list.reserve(traffic.vehicles().size() + 1);
  for (const auto& v : traffic.vehicles()) list.push_back(&v);
  if (extra) list.push_back(extra);  // the player's car
  for (const Vehicle* vp : list) {
    const Vehicle& v = *vp;
    if (std::hypot(v.pos.x - c.x, v.pos.y - c.y) > 320.0) continue;
    const VehicleModel& m = vehicles_.get(v.type);
    const Matrix M = vehicleMatrix(v);
    if (cockpit && vp == extra) {  // from the driver's seat
      const Color paint{static_cast<unsigned char>(std::sqrt(v.color[0]) * 255), static_cast<unsigned char>(std::sqrt(v.color[1]) * 255),
                        static_cast<unsigned char>(std::sqrt(v.color[2]) * 255), 255};
      drawMeshMat(m.cockpit, M, -1, WHITE);
      drawMeshMat(m.bonnet, M, -1, paint);
      const Vector3 sp = modelToRl(m.steer_pos);
      const float wheel_turn = std::clamp(cockpit->steer * 15.0f, -9.5f, 9.5f);  // steering ratio ~15:1
      const Matrix S = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateZ(-wheel_turn), MatrixRotateX(m.steer_tilt)),
                                                     MatrixTranslate(sp.x, sp.y, sp.z)), M);
      drawMeshMat(m.steering, S, -1, WHITE);
      const float glow = 0.5f + 1.2f * L.night;
      drawMeshMat(m.gauges, M, -1, WHITE, Vector3{glow, glow, glow * 0.95f});
      const float vals[2] = {std::clamp(std::fabs(cockpit->kmh) / 180.0f, 0.0f, 1.0f), std::clamp(cockpit->rpm / 8000.0f, 0.0f, 1.0f)};
      for (int k = 0; k < 2; ++k) {
        const Vector3 gp = modelToRl(m.gauge_pos[k]);
        const float a = (-120.0f + 240.0f * vals[k]) * DEG2RAD;
        const Matrix N = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a), MatrixTranslate(gp.x, gp.y, gp.z)), M);
        drawMeshMat(m.needle, N, -1, WHITE, Vector3Scale(Vector3{1.0f, 0.32f, 0.06f}, 1.2f + 2.0f * L.night));
      }
      if (mirror_ok_) {
        // interior mirror: centre of the rear view; door mirrors: the outer parts on their side
        const float crops[3][2] = {{0.24f, 0.76f}, {0.0f, 0.34f}, {0.66f, 1.0f}};
        for (int k = 0; k < 3; ++k) {
          Vector3 q[4];
          for (int i = 0; i < 4; ++i) q[i] = Vector3Transform(modelToRl(m.mirror_glass[k][i]), M);
          drawMirrorGlass(q, crops[k][0], crops[k][1]);
        }
      }
      continue;
    }
    const Color paint{static_cast<unsigned char>(std::sqrt(v.color[0]) * 255), static_cast<unsigned char>(std::sqrt(v.color[1]) * 255),
                      static_cast<unsigned char>(std::sqrt(v.color[2]) * 255), 255};
    // body: per-vertex materials (paint tinted by colDiffuse, glass, trim, plates)
    setI(lit_, "materialOverride", -1);
    setI(lit_, "useTexture", 0);
    setI(lit_, "surfaceMode", 0);
    mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = paint;
    DrawMesh(m.body, mat_, M);
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    ++draw_calls_;
    triangles_ += m.body.triangleCount;
    // wheels: turn with the distance rolled, the front pair steers; the left pair is mirrored
    const Matrix Mw = vehicleMatrix(v, false);
    const float spin = -v.wheel_dist / m.wheel_r;
    for (const auto& wp : m.wheel_pos) {
      const float side = wp[0] > 0 ? 1.0f : -1.0f;
      Matrix W = MatrixMultiply(MatrixScale(side, 1.0f, 1.0f), MatrixRotateX(spin));
      if (wp[1] > 0) W = MatrixMultiply(W, MatrixRotateY(-v.steer));
      W = MatrixMultiply(W, MatrixTranslate(wp[0], m.wheel_r, -wp[1]));
      drawMeshMat(m.wheel, MatrixMultiply(W, Mw), -1, WHITE);
    }
    // the driver (right-hand drive): seated behind the wheel, visible through the windows
    if ((vp != extra || extra_driven) && humans_.ready() && std::hypot(v.pos.x - c.x, v.pos.y - c.y) < 70.0) {
      static const Color shirts[] = {{235, 235, 232, 255}, {40, 52, 84, 255}, {30, 30, 32, 255}, {128, 130, 134, 255}, {150, 180, 210, 255},
                                     {118, 40, 44, 255},   {196, 180, 150, 255}};
      static const Color skins[] = {{236, 204, 176, 255}, {222, 186, 150, 255}, {204, 166, 132, 255}};
      static const Color hairs[] = {{22, 18, 16, 255}, {30, 24, 20, 255}, {58, 40, 28, 255}, {150, 150, 150, 255}};
      const uint32_t h = static_cast<uint32_t>(v.id) * 2654435761u + (vp == extra ? 7u : 0u);
      const bool uniform = v.type == VehicleType::Bus || v.type == VehicleType::Taxi;  // bus and taxi drivers wear uniforms
      const Color top = uniform ? Color{40, 50, 78, 255} : shirts[(h >> 5) % 7];
      const DriverSeat ds = driverSeat(v.type);
      // eye (ds) -> seated figure's origin: hips 0.18 m behind the eye, eyes ~1.3 m above the origin
      const Matrix P = MatrixMultiply(MatrixTranslate(ds.side, ds.up - 1.3f, -(ds.fwd - 0.18f)), M);
      drawHuman(humans_.still(static_cast<BodyVariant>((h >> 11) % static_cast<int>(BodyVariant::Count)), StillPose::Sit), P, top,
                Color{38, 40, 48, 255}, skins[(h >> 15) % 3], hairs[(h >> 19) % 4]);
    }
    // lamps
    const float night = L.night;
    const Vector3 head = Vector3Scale(Vector3{1.0f, 0.96f, 0.88f}, 0.4f + 7.0f * night);
    const float tail_k = (v.braking ? 5.0f : 0.0f) + 0.25f + 1.6f * night;
    drawMeshMat(m.head_lamps, M, kMatSignalLamp, WHITE, head);
    drawMeshMat(m.tail_lamps, M, kMatSignalLamp, WHITE, Vector3Scale(Vector3{1.0f, 0.05f, 0.03f}, tail_k));
    // turn indicators: amber, about 85 flashes a minute while turning; dim lens otherwise
    const bool flash = std::fmod(GetTime() + v.id * 0.13, 0.7) < 0.35;
    for (int k = 0; k < 2; ++k) {
      const bool on = flash && v.blink == (k == 0 ? -1 : 1);
      drawMeshMat(m.indicators[k], M, kMatSignalLamp, WHITE, on ? Vector3{6.0f, 2.6f, 0.2f} : Vector3{0.12f, 0.06f, 0.01f});
    }
  }
  rlEnableBackfaceCulling();
}

void Renderer::drawHuman(const Mesh& m, const Matrix& model, Color top, Color bottom, Color skin, Color hair) {
  auto c3 = [](Color c) { return Vector3{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}; };
  set3(lit_, "partTop", c3(top));
  set3(lit_, "partBottom", c3(bottom));
  set3(lit_, "partSkin", c3(skin));
  set3(lit_, "partHair", c3(hair));
  DrawMesh(m, mat_, model);
  ++draw_calls_;
  triangles_ += m.triangleCount;
}

void Renderer::buildStationSigns(const Trains& trains, const Font& font) {
  if (signs_built_ || !trains.loaded()) return;
  const auto& st = trains.stations();
  constexpr int W = 640, H = 200;
  for (size_t i = 0; i < st.size(); ++i) {
    const Station& s = st[i];
    const auto kind = trains.lines()[static_cast<size_t>(s.line)].kind;
    const Color band = kind == LineKind::Shinkansen ? Color{26, 64, 160, 255} : kind == LineKind::Branch ? Color{224, 118, 30, 255} : Color{28, 150, 128, 255};
    // neighbours: the stations before and after this one along the line
    int prev = -1, next = -1;
    double dp = 1e30, dn = 1e30;
    const double len = trains.lines()[static_cast<size_t>(s.line)].length;
    const bool closed = trains.lines()[static_cast<size_t>(s.line)].closed;
    for (size_t j = 0; j < st.size(); ++j) {
      if (j == i || st[j].line != s.line) continue;
      double d = st[j].s - s.s;
      if (closed) d = std::remainder(d, len);
      if (d > 0 && d < dn) dn = d, next = static_cast<int>(j);
      if (d < 0 && -d < dp) dp = -d, prev = static_cast<int>(j);
    }
    RenderTexture2D rt = LoadRenderTexture(W, H);
    SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
    BeginTextureMode(rt);
    ClearBackground(Color{246, 246, 244, 255});
    const std::string base = stationBaseName(s.name);
    const StationReading* rd = stationReading(s.name);
    auto centred = [&](const std::string& t, float y, float size, Color c) {
      const Vector2 m = MeasureTextEx(font, t.c_str(), size, 1.0f);
      DrawTextEx(font, t.c_str(), Vector2{(W - m.x) * 0.5f, y}, size, 1.0f, c);
    };
    centred(base, 10, 74, Color{28, 28, 30, 255});
    if (rd) centred(rd->kana, 90, 28, Color{40, 40, 44, 255});
    DrawRectangle(0, 126, W, H - 126, band);
    if (rd) centred(rd->roman, 131, 24, WHITE);
    if (prev >= 0) DrawTextEx(font, ("← " + stationBaseName(st[static_cast<size_t>(prev)].name)).c_str(), Vector2{14, 162}, 26, 1.0f, WHITE);
    if (next >= 0) {
      const std::string t = stationBaseName(st[static_cast<size_t>(next)].name) + " →";
      const Vector2 m = MeasureTextEx(font, t.c_str(), 26, 1.0f);
      DrawTextEx(font, t.c_str(), Vector2{W - 14 - m.x, 162}, 26, 1.0f, WHITE);
    }
    EndTextureMode();
    signs_.push_back(rt);
  }
  signs_built_ = true;
  TraceLog(LOG_INFO, "RJ: station name boards built (%zu)", signs_.size());
}

void Renderer::drawStationSigns(const Trains& trains, const Camera3D& cam) {
  if (!signs_built_) return;
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  const auto& st = trains.stations();
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, false);  // keep the scene's reflection mask (alpha) untouched
  rlDisableBackfaceCulling();
  for (size_t i = 0; i < st.size() && i < signs_.size(); ++i) {
    const Station& s = st[i];
    if (std::hypot(s.pos.x - c.x, s.pos.y - c.y) > 260.0) continue;
    const auto kind = trains.lines()[static_cast<size_t>(s.line)].kind;
    const double off = Trains::platformOffset(kind), plat_len = kind == LineKind::Shinkansen ? 320.0 : 200.0;
    rlSetTexture(signs_[i].texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(232, 232, 232, 255);
    for (double side : {-1.0, 1.0}) {  // both platforms (right = +)
      for (double u = -plat_len * 0.35; u <= plat_len * 0.36; u += plat_len * 0.35) {
        rj::geo::Vec3d p;
        double h;
        trains.poseAt(s.line, s.s + u, p, h);
        const double fx = std::sin(h), fy = std::cos(h), rx = fy, ry = -fx;
        const double lat = side * (off - 2.5 + 1.3);  // 1.3 m back from the platform edge
        const rj::geo::Vec3d ctr{p.x + rx * lat, p.y + ry * lat, s.pos.z + 2.45};
        if (std::hypot(ctr.x - c.x, ctr.y - c.y) > 150.0) continue;
        // the face towards the track reads left to right for someone looking from the track;
        // the back face (towards the platform) is printed the same way round for its viewers
        for (int face = 0; face < 2; ++face) {
          const double o = (face == 0 ? -side : side) * 0.012;  // the two faces 2.4 cm apart
          const double ux = face == 0 ? -side * fx : side * fx, uy = face == 0 ? -side * fy : side * fy;  // text +u
          auto V = [&](double a, double b) {  // a: -0.8..0.8 along u, b: -0.25..0.25 up
            const Vector3 q = enuToRl({ctr.x + rx * o + ux * a, ctr.y + ry * o + uy * a, ctr.z + b});
            rlVertex3f(q.x, q.y, q.z);
          };
          // render-texture rows run bottom-up: v = 1 at the top of the board
          rlTexCoord2f(0, 0);
          V(-0.8, -0.25);
          rlTexCoord2f(1, 0);
          V(0.8, -0.25);
          rlTexCoord2f(1, 1);
          V(0.8, 0.25);
          rlTexCoord2f(0, 1);
          V(-0.8, 0.25);
        }
      }
      // and boards hung across the platform (read by people walking along it)
      for (double u : {-plat_len * 0.18, plat_len * 0.18}) {
        rj::geo::Vec3d p;
        double h;
        trains.poseAt(s.line, s.s + u, p, h);
        const double fx = std::sin(h), fy = std::cos(h), rx = fy, ry = -fx;
        const rj::geo::Vec3d ctr{p.x + rx * side * off, p.y + ry * side * off, s.pos.z + 2.6};
        if (std::hypot(ctr.x - c.x, ctr.y - c.y) > 150.0) continue;
        for (int face = 0; face < 2; ++face) {
          const double fs = face == 0 ? 1.0 : -1.0, o = fs * 0.012;
          auto V = [&](double a, double b) {  // text +u to the right of someone facing this face
            const Vector3 q = enuToRl({ctr.x + fx * o - fs * rx * a, ctr.y + fy * o - fs * ry * a, ctr.z + b});
            rlVertex3f(q.x, q.y, q.z);
          };
          rlTexCoord2f(0, 0);
          V(-0.8, -0.25);
          rlTexCoord2f(1, 0);
          V(0.8, -0.25);
          rlTexCoord2f(1, 1);
          V(0.8, 0.25);
          rlTexCoord2f(0, 1);
          V(-0.8, 0.25);
        }
      }
    }
    rlEnd();
    rlDrawRenderBatchActive();
  }
  rlSetTexture(0);
  rlColorMask(true, true, true, true);
  rlEnableBackfaceCulling();
}

void Renderer::setDepartureBoard(int station, int side, const std::string& type, const std::string& dest, const std::string& when,
                                 const std::string& notice, const Font& font) {
  constexpr int W = 512, H = 128;
  DepartureBoard* b = nullptr;
  for (auto& x : boards_)
    if (x.station == station && x.side == side) b = &x;
  if (!b) {
    boards_.push_back({station, side, "", LoadRenderTexture(W * 2, H), 0.0f});  // twice as wide: the notice line scrolls
    b = &boards_.back();
    SetTextureFilter(b->rt.texture, TEXTURE_FILTER_BILINEAR);
  }
  const std::string key = type + "|" + dest + "|" + when + "|" + notice;
  if (key == b->key) return;
  b->key = key;
  BeginTextureMode(b->rt);
  ClearBackground(Color{8, 8, 10, 255});
  const Color amber{255, 150, 40, 255}, green{80, 230, 110, 255}, white{235, 235, 225, 255};
  DrawTextEx(font, type.c_str(), Vector2{14, 10}, 44, 1.0f, green);
  DrawTextEx(font, dest.c_str(), Vector2{150, 10}, 44, 1.0f, amber);
  const Vector2 wm = MeasureTextEx(font, when.c_str(), 40, 1.0f);
  DrawTextEx(font, when.c_str(), Vector2{W - 14 - wm.x, 12}, 40, 1.0f, amber);
  // the notice line is drawn once at the left of the double-width texture; drawing scrolls it
  const Vector2 nm = MeasureTextEx(font, notice.c_str(), 34, 1.0f);
  b->notice_w = nm.x;
  DrawTextEx(font, notice.c_str(), Vector2{14, 72}, 34, 1.0f, white);
  // LED dot matrix: dark gaps between the rows and columns of dots
  for (int y = 0; y < H; y += 4) DrawRectangle(0, y, W * 2, 1, Color{0, 0, 0, 150});
  for (int x = 0; x < W * 2; x += 4) DrawRectangle(x, 0, 1, H, Color{0, 0, 0, 110});
  EndTextureMode();
}

void Renderer::drawDepartureBoards(const Trains& trains, const Camera3D& cam, float time_s) {
  if (boards_.empty()) return;
  const rj::geo::Vec3d c = rlToEnu(cam.position);
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, false);
  rlDisableBackfaceCulling();
  for (const auto& b : boards_) {
    if (b.station < 0 || b.station >= static_cast<int>(trains.stations().size())) continue;
    const Station& s = trains.stations()[static_cast<size_t>(b.station)];
    const auto kind = trains.lines()[static_cast<size_t>(s.line)].kind;
    rj::geo::Vec3d p;
    double h;
    trains.poseAt(s.line, s.s + 8.0, p, h);
    const double fx = std::sin(h), fy = std::cos(h), rx = fy, ry = -fx, lat = b.side * Trains::platformOffset(kind);
    const rj::geo::Vec3d ctr{p.x + rx * lat, p.y + ry * lat, s.pos.z + 2.75};
    if (std::hypot(ctr.x - c.x, ctr.y - c.y) > 120.0) continue;
    // the top line is fixed, the notice line scrolls when it is longer than the board
    const float scroll = b.notice_w > 480.0f ? std::fmod(time_s * 90.0f, b.notice_w + 520.0f) - 480.0f : 0.0f;
    const float u1 = 0.5f;  // the visible half of the double-width texture
    const float us = std::max(0.0f, scroll) / 1024.0f;
    rlSetTexture(b.rt.texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    for (int face = 0; face < 2; ++face) {  // two faces, one towards each end of the platform
      const double fs = face == 0 ? 1.0 : -1.0;
      const double ux = -fs * rx, uy = -fs * ry;  // text +u: to the right of someone facing this face
      const double o = fs * 0.02;
      auto V = [&](double a, double z) {
        const Vector3 q = enuToRl({ctr.x + fx * o + ux * a, ctr.y + fy * o + uy * a, ctr.z + z});
        rlVertex3f(q.x, q.y, q.z);
      };
      // top line (rows 0..64 of the texture: v 1 .. 0.5), notice line (v 0.5 .. 0) with scrolling
      rlTexCoord2f(0, 0.5f);
      V(-0.9, 0.0);
      rlTexCoord2f(u1, 0.5f);
      V(0.9, 0.0);
      rlTexCoord2f(u1, 1.0f);
      V(0.9, 0.225);
      rlTexCoord2f(0, 1.0f);
      V(-0.9, 0.225);
      rlTexCoord2f(us, 0.0f);
      V(-0.9, -0.225);
      rlTexCoord2f(us + u1, 0.0f);
      V(0.9, -0.225);
      rlTexCoord2f(us + u1, 0.5f);
      V(0.9, 0.0);
      rlTexCoord2f(us, 0.5f);
      V(-0.9, 0.0);
    }
    rlEnd();
    rlDrawRenderBatchActive();
  }
  rlSetTexture(0);
  rlColorMask(true, true, true, true);
  rlEnableBackfaceCulling();
}

void Renderer::setCarDisplay(const std::string& text, const Font& font) {
  constexpr int W = 768, H = 96;
  if (!car_display_.id) {
    car_display_ = LoadRenderTexture(W, H);
    SetTextureFilter(car_display_.texture, TEXTURE_FILTER_BILINEAR);
  }
  if (text == car_display_key_) return;
  car_display_key_ = text;
  BeginTextureMode(car_display_);
  ClearBackground(Color{6, 6, 8, 255});
  float size = 58.0f;
  Vector2 m = MeasureTextEx(font, text.c_str(), size, 1.0f);
  if (m.x > W - 24) {
    size *= (W - 24) / m.x;
    m = MeasureTextEx(font, text.c_str(), size, 1.0f);
  }
  DrawTextEx(font, text.c_str(), Vector2{(W - m.x) * 0.5f, (H - m.y) * 0.5f}, size, 1.0f, Color{255, 150, 40, 255});
  for (int y = 0; y < H; y += 4) DrawRectangle(0, y, W, 1, Color{0, 0, 0, 140});
  for (int x = 0; x < W; x += 4) DrawRectangle(x, 0, 1, H, Color{0, 0, 0, 100});
  EndTextureMode();
  if (std::getenv("RJ_DEBUG_DISPLAY")) {
    Image im = LoadImageFromTexture(car_display_.texture);
    ExportImage(im, "car_display.png");
    UnloadImage(im);
    TraceLog(LOG_INFO, "RJ: car display '%s' (font %d glyphs, size %.0f)", text.c_str(), font.glyphCount, size);
  }
}

void Renderer::drawCarDisplay(const Trains& trains, int ride_train, int ride_car) {
  const Train* t = ride_train >= 0 ? trains.train(ride_train) : nullptr;
  if (!t || !car_display_.id || car_display_key_.empty()) return;
  const bool shink = trains.lines()[static_cast<size_t>(t->line)].kind == LineKind::Shinkansen;
  rj::geo::Vec3d p;
  float yaw, pitch;
  trains.carPose(*t, ride_car, p, yaw, pitch);
  const bool reversed = ride_car == t->cars - 1 && ride_car > 0;  // the rear cab is turned round
  if (reversed) yaw += PI, pitch = -pitch;
  const double fx = std::sin(yaw), fy = std::cos(yaw), rx = fy, ry = -fx, tp = std::tan(pitch);
  auto W = [&](double x, double y, double z) {  // car model -> raylib
    return enuToRl({p.x + rx * x + fx * y, p.y + ry * x + fy * y, p.z + z + tp * y});
  };
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, false);
  rlDisableBackfaceCulling();
  rlSetTexture(car_display_.texture.id);
  rlBegin(RL_QUADS);
  rlColor4ub(255, 255, 255, 255);
  // quad: centre (x, y, z), text +u along (ux, uy) in the car's plane, half size (hw, hh)
  auto Q = [&](double x, double y, double z, double ux, double uy, double hw, double hh) {
    const Vector3 a = W(x - ux * hw, y - uy * hw, z - hh), b = W(x + ux * hw, y + uy * hw, z - hh), c = W(x + ux * hw, y + uy * hw, z + hh),
                  d = W(x - ux * hw, y - uy * hw, z + hh);
    rlTexCoord2f(0, 0);
    rlVertex3f(a.x, a.y, a.z);
    rlTexCoord2f(1, 0);
    rlVertex3f(b.x, b.y, b.z);
    rlTexCoord2f(1, 1);
    rlVertex3f(c.x, c.y, c.z);
    rlTexCoord2f(0, 1);
    rlVertex3f(d.x, d.y, d.z);
  };
  if (shink) {
    const bool nose = ride_car == 0 || ride_car == t->cars - 1;
    const double ye = (nose ? 12.5 - 11.5 : 12.5) - 0.3 - 0.042;
    Q(0.0, ye, 1.15 + 2.12, 1.0, 0.0, 0.38, 0.065);  // on the end wall, facing the seats
  } else {
    for (int d = 0; d < 4; ++d) {
      const double yc = -10.0 + 2.45 + d * 5.03;
      Q(1.354, yc, 2.95 + 0.07, 0.0, -1.0, 0.27, 0.044);  // over the door (door top 2.95 m); right wall reads towards the back
      Q(-1.354, yc, 2.95 + 0.07, 0.0, 1.0, 0.27, 0.044);
    }
  }
  rlEnd();
  rlSetTexture(0);
  rlDrawRenderBatchActive();
  rlColorMask(true, true, true, true);
  rlEnableBackfaceCulling();
}

void Renderer::drawCrowd(const std::vector<CrowdPerson>& people) {
  if (people.empty()) return;
  setI(lit_, "materialOverride", -1);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  rlDisableBackfaceCulling();
  bool lit_inside = false;
  for (const auto& p : people) {
    if (p.inside != lit_inside) {  // people in a car are lit by its lights, like the interior
      lit_inside = p.inside;
      set3(lit_, "selfLight", lit_inside ? kCabinLight : Vector3{0, 0, 0});
    }
    const BodyVariant v = static_cast<BodyVariant>(p.variant % static_cast<int>(BodyVariant::Count));
    const Mesh& m = p.pose == 2 ? humans_.still(v, StillPose::Sit) : p.pose == 3 ? humans_.still(v, StillPose::Strap) : humans_.frame(v, p.phase, p.pose == 0);
    const Vector3 feet = enuToRl(p.pos);
    Matrix R = MatrixRotateY(-p.face);
    if (p.pitch != 0.0f || p.roll != 0.0f) R = MatrixMultiply(MatrixMultiply(R, MatrixRotateZ(-p.roll)), MatrixRotateX(p.pitch));  // with the vehicle
    const Matrix M = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(p.scale, p.scale, p.scale), R), MatrixRotateY(-p.yaw)),
                                    MatrixTranslate(feet.x, feet.y, feet.z));
    drawHuman(m, M, p.top, p.bottom, p.skin, p.hair);
  }
  if (lit_inside) set3(lit_, "selfLight", Vector3{0, 0, 0});
  rlEnableBackfaceCulling();
}

void Renderer::drawPedestrians(const Pedestrians& peds, float rain) {
  setI(lit_, "materialOverride", -1);
  setI(lit_, "useTexture", 0);
  setI(lit_, "surfaceMode", 0);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  rlDisableBackfaceCulling();
  for (const auto& [id, w] : peds.walkers()) {
    const float s = w.height_scale;
    const Vector3 feet = enuToRl({w.pos.x, w.pos.y, static_cast<double>(w.z)});
    const Matrix M = MatrixMultiply(MatrixMultiply(MatrixScale(s, s, s), MatrixRotateY(-w.yaw)), MatrixTranslate(feet.x, feet.y, feet.z));
    const Mesh& m = humans_.frame(static_cast<BodyVariant>(w.variant % static_cast<int>(BodyVariant::Count)), w.phase, w.waiting);
    drawHuman(m, M, w.shirt, w.pants, w.skin, w.hair);
    // In the rain most people carry umbrellas; clear vinyl ones are the most common in Tokyo.
    const uint32_t h = static_cast<uint32_t>(id * 2654435761u);
    if (rain > 0.12f && (h & 1023u) < static_cast<uint32_t>(std::min(1.0f, 0.55f + rain) * 1023.0f)) {
      static const Color kUmbrella[] = {{226, 230, 234, 255}, {226, 230, 234, 255}, {226, 230, 234, 255}, {24, 24, 28, 255},
                                        {30, 38, 66, 255},    {176, 156, 128, 255}, {120, 30, 34, 255},   {60, 90, 70, 255}};
      const Color uc = kUmbrella[(h >> 10) % 8];
      set3(lit_, "partTop", Vector3{uc.r / 255.0f, uc.g / 255.0f, uc.b / 255.0f});
      DrawMesh(humans_.umbrella(), mat_, M);
      ++draw_calls_;
    }
  }
  rlEnableBackfaceCulling();
}

void Renderer::drawRain(const Camera3D& cam, const Lighting& L, float time_s, float snow) {
  if (L.rain <= 0.01f || L.indoor > 0.5f) return;
  beginTransparent();
  rlColorMask(true, true, true, false);  // keep the scene's reflection mask (alpha): drops and flakes do not reflect
  if (snow > 0.5f) {
    // snowflakes: slow, drifting, swaying; a 28 m volume snapped to the camera like the rain's
    const int n = static_cast<int>(900 + 3600 * L.rain);
    const Color col{236, 240, 246, static_cast<unsigned char>(150 + 80 * L.rain)};
    rlSetBlendMode(BLEND_ALPHA);
    rlDisableDepthMask();
    const float S = 28.0f;
    const float gx = std::floor(cam.position.x / S) * S, gz = std::floor(cam.position.z / S) * S;
    for (int i = 0; i < n; ++i) {
      const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
      const float rx = ((h & 1023u) / 1023.0f - 0.5f) * S;
      const float rz = (((h >> 10) & 1023u) / 1023.0f - 0.5f) * S;
      const float ph = ((h >> 20) & 1023u) / 1023.0f;
      const float fall = 0.9f + 0.5f * (((h >> 5) & 255u) / 255.0f);
      const float y = 12.0f - std::fmod(ph * 14.0f + time_s * fall, 14.0f);
      const float sway = std::sin(time_s * 0.9f + ph * 17.0f) * 0.45f;
      float px = gx + rx + sway + time_s * 0.6f * L.wind, pz = gz + rz + std::cos(time_s * 0.7f + ph * 11.0f) * 0.35f;
      px = cam.position.x + std::remainder(px - cam.position.x, S);
      pz = cam.position.z + std::remainder(pz - cam.position.z, S);
      const Vector3 p{px, cam.position.y - 3.0f + y, pz};
      DrawLine3D(p, Vector3{p.x + 0.035f, p.y - 0.035f, p.z + 0.02f}, col);
      DrawLine3D(Vector3{p.x + 0.035f, p.y, p.z}, Vector3{p.x, p.y - 0.035f, p.z + 0.02f}, col);
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlColorMask(true, true, true, true);
    return;
  }
  const int n = static_cast<int>(600 + 2600 * L.rain);
  const float speed = 8.5f, len = 0.35f + 0.5f * L.rain;
  const Color col{190, 198, 210, static_cast<unsigned char>(60 + 60 * L.rain)};
  rlSetBlendMode(BLEND_ALPHA);
  rlDisableDepthMask();
  for (int i = 0; i < n; ++i) {
    const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
    const float rx = ((h & 1023u) / 1023.0f - 0.5f) * 36.0f;
    const float rz = (((h >> 10) & 1023u) / 1023.0f - 0.5f) * 36.0f;
    const float ph = ((h >> 20) & 1023u) / 1023.0f;
    const float y = 14.0f - std::fmod(ph * 16.0f + time_s * speed, 16.0f);
    // snap the rain volume to a 36 m grid so drops stay put while the camera moves
    const float gx = std::floor(cam.position.x / 36.0f) * 36.0f, gz = std::floor(cam.position.z / 36.0f) * 36.0f;
    float px = gx + rx, pz = gz + rz;
    if (px < cam.position.x - 18.0f) px += 36.0f;
    if (pz < cam.position.z - 18.0f) pz += 36.0f;
    const Vector3 top{px, cam.position.y - 3.0f + y, pz};
    DrawLine3D(top, Vector3{top.x + 0.06f, top.y - len, top.z + 0.03f}, col);
  }
  rlDrawRenderBatchActive();
  rlEnableDepthMask();
  rlColorMask(true, true, true, true);
}

void Renderer::fullscreen(Shader& sh, const Texture2D& src, RenderTexture2D& dst, bool flip_src) {
  BeginTextureMode(dst);
  ClearBackground(BLACK);
  rlDisableColorBlend();
  BeginShaderMode(sh);
  const float sh_ = flip_src ? -static_cast<float>(src.height) : static_cast<float>(src.height);
  DrawTexturePro(src, Rectangle{0, 0, static_cast<float>(src.width), sh_},
                 Rectangle{0, 0, static_cast<float>(dst.texture.width), static_cast<float>(dst.texture.height)}, Vector2{0, 0}, 0.0f,
                 WHITE);
  EndShaderMode();
  rlDrawRenderBatchActive();
  rlEnableColorBlend();
  EndTextureMode();
}

namespace {
Vector3 toneMapCpu(Vector3 c, float e) {
  auto f = [&](float x) {
    x *= e;
    const float y = std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    return std::pow(y, 1.0f / 2.2f);
  };
  return {f(c.x), f(c.y), f(c.z)};
}
}  // namespace

void Renderer::endScene(const Camera3D& cam, const Lighting& L, float time_s) {
  rlDrawRenderBatchActive();
  rlEnableColorBlend();
  if (!post_active_) return;
  EndTextureMode();
  const float aspect = static_cast<float>(tw_) / static_cast<float>(std::max(1, th_));
  const float tan_half = std::tan(cam.fovy * DEG2RAD * 0.5f);
  // SSAO (half resolution) from the scene depth.
  if (opt_.ssao) {
    Texture2D depth{scene_.depth.id, tw_, th_, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    set2(ssao_, "invRes", Vector2{1.0f / tw_, 1.0f / th_});
    setF(ssao_, "nearZ", g_near);
    setF(ssao_, "farZ", g_far);
    set2(ssao_, "tanHalf", Vector2{tan_half * aspect, tan_half});
    fullscreen(ssao_, depth, ao_, false);
    set2(blur_, "dir", Vector2{1.0f / ao_.texture.width, 0.0f});
    fullscreen(blur_, ao_.texture, ao_blur_, false);
    set2(blur_, "dir", Vector2{0.0f, 1.0f / ao_.texture.height});
    fullscreen(blur_, ao_blur_.texture, ao_, false);
  } else {
    BeginTextureMode(ao_);
    ClearBackground(WHITE);
    EndTextureMode();
  }
  // Screen-space reflections (half resolution) for glass, puddles and wet asphalt.
  {
    const float expo = exposure_override_ > 0.0f ? exposure_override_ : exposure_;
    Texture2D depth{scene_.depth.id, tw_, th_, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    BeginTextureMode(ssr_rt_);
    ClearBackground(BLANK);
    rlDisableColorBlend();
    BeginShaderMode(ssr_);
    SetShaderValueTexture(ssr_, GetShaderLocation(ssr_, "texDepth"), depth);
    set2(ssr_, "invRes", Vector2{1.0f / tw_, 1.0f / th_});
    setF(ssr_, "nearZ", g_near);
    setF(ssr_, "farZ", g_far);
    set2(ssr_, "tanHalf", Vector2{tan_half * aspect, tan_half});
    SetShaderValueMatrix(ssr_, GetShaderLocation(ssr_, "invView"), MatrixInvert(GetCameraMatrix(cam)));
    set3(ssr_, "fbZenith", toneMapCpu(L.sky_zenith, expo));
    set3(ssr_, "fbHorizon", toneMapCpu(L.sky_horizon, expo));
    set3(ssr_, "fbCity", toneMapCpu(Vector3Add(Vector3Scale(L.ambient_ground, 0.75f), Vector3Scale(L.ambient_sky, 0.15f)), expo));
    DrawTexturePro(scene_.texture, Rectangle{0, 0, static_cast<float>(tw_), static_cast<float>(th_)},
                   Rectangle{0, 0, static_cast<float>(ssr_rt_.texture.width), static_cast<float>(ssr_rt_.texture.height)}, Vector2{0, 0}, 0.0f,
                   WHITE);
    EndShaderMode();
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
    EndTextureMode();
  }
  // Bloom (quarter resolution): bright pass + two separable blur iterations.
  if (opt_.bloom) {
    setF(bright_, "threshold", 0.82f);
    fullscreen(bright_, scene_.texture, bright_rt_, false);
    const Vector2 hx{1.0f / bloom_a_.texture.width, 0.0f}, vy{0.0f, 1.0f / bloom_a_.texture.height};
    set2(blur_, "dir", hx);
    fullscreen(blur_, bright_rt_.texture, bloom_a_, false);
    set2(blur_, "dir", vy);
    fullscreen(blur_, bloom_a_.texture, bloom_b_, false);
    set2(blur_, "dir", Vector2Scale(hx, 2.0f));
    fullscreen(blur_, bloom_b_.texture, bloom_a_, false);
    set2(blur_, "dir", Vector2Scale(vy, 2.0f));
    fullscreen(blur_, bloom_a_.texture, bloom_b_, false);
  } else {
    BeginTextureMode(bloom_b_);
    ClearBackground(BLACK);
    EndTextureMode();
  }
  // Auto exposure: average display luminance of a tiny downsample, corrected towards a
  // time-of-day target (night stays dark; the eye adapts but not fully).
  if (frame_ % 4 == 0 && exposure_override_ <= 0.0f) {
    BeginTextureMode(lum_);
    rlDisableColorBlend();
    DrawTexturePro(scene_.texture, Rectangle{0, 0, static_cast<float>(tw_), static_cast<float>(th_)}, Rectangle{0, 0, 32, 18},
                   Vector2{0, 0}, 0.0f, WHITE);
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
    EndTextureMode();
    Image img = LoadImageFromTexture(lum_.texture);
    if (img.data) {
      const unsigned char* p = static_cast<const unsigned char*>(img.data);
      double sum = 0;
      const int n = img.width * img.height;
      for (int i = 0; i < n; ++i) {
        const double r = std::pow(p[i * 4] / 255.0, 2.2), g = std::pow(p[i * 4 + 1] / 255.0, 2.2), b = std::pow(p[i * 4 + 2] / 255.0, 2.2);
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
      }
      UnloadImage(img);
      const double mean = std::max(sum / std::max(1, n), 1e-4);
      const double target = 0.155 - 0.085 * L.night * (1.0 - L.indoor);
      const double k = std::pow(target / mean, 0.45);
      exposure_ = static_cast<float>(std::clamp(exposure_ * std::clamp(k, 0.8, 1.25), 0.45, 4.5));
    }
  }
  // Composite to the backbuffer (opaque: the scene alpha channel holds SSR data, not coverage).
  rlDrawRenderBatchActive();
  rlDisableColorBlend();
  BeginShaderMode(composite_);
  SetShaderValueTexture(composite_, GetShaderLocation(composite_, "texAO"), ao_.texture);
  SetShaderValueTexture(composite_, GetShaderLocation(composite_, "texBloom"), bloom_b_.texture);
  SetShaderValueTexture(composite_, GetShaderLocation(composite_, "texSsr"), ssr_rt_.texture);
  setF(composite_, "aoStrength", opt_.ssao ? 0.85f : 0.0f);
  setF(composite_, "bloomStrength", opt_.bloom ? 0.35f + 0.45f * L.night : 0.0f);
  set3(composite_, "whiteBalance", L.white_balance);
  setF(composite_, "saturation", L.saturation);
  setF(composite_, "contrast", L.contrast);
  setF(composite_, "vignette", 0.22f);
  setF(composite_, "rainOverlay", L.rain * (1.0f - L.indoor));
  setF(composite_, "timeSec", time_s);
  set2(composite_, "invRes", Vector2{1.0f / tw_, 1.0f / th_});
  static const int dbg = std::getenv("RJ_DEBUG_VIEW") ? std::atoi(std::getenv("RJ_DEBUG_VIEW")) : 0;
  setI(composite_, "debugView", dbg);
  DrawTexturePro(scene_.texture, Rectangle{0, 0, static_cast<float>(tw_), -static_cast<float>(th_)},
                 Rectangle{0, 0, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())}, Vector2{0, 0}, 0.0f,
                 WHITE);
  EndShaderMode();
  rlDrawRenderBatchActive();
  rlEnableColorBlend();
}

}  // namespace rjc
