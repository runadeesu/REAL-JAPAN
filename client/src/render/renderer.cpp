#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>

#include "raymath.h"
#include "rlgl.h"
#include "world/coords.hpp"

namespace rjc {
namespace {

const char* kLitVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPos;
out vec3 fragNormal;
out vec4 fragColor;
out vec2 fragUV;
void main() {
  fragPos = vec3(matModel * vec4(vertexPosition, 1.0));
  fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
  fragColor = vertexColor;
  fragUV = vertexTexCoord;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kLitFs = R"(#version 330
in vec3 fragPos;
in vec3 fragNormal;
in vec4 fragColor;
in vec2 fragUV;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform int useTexture;
uniform vec3 sunDir;
uniform vec3 sunColor;
uniform vec3 ambientSky;
uniform vec3 ambientGround;
uniform vec3 fogColor;
uniform float fogDensity;
uniform vec3 viewPos;
uniform mat4 lightVP;
uniform sampler2D shadowMap;
uniform int shadowsOn;
uniform float shadowTexel;
out vec4 finalColor;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }
vec3 aces(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
float shadowFactor(vec3 n) {
  vec4 ls = lightVP * vec4(fragPos + n * 0.15, 1.0);
  vec3 p = ls.xyz / ls.w * 0.5 + 0.5;
  if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;
  float bias = 0.0006;
  float lit = 0.0;
  for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++) {
      float d = texture(shadowMap, p.xy + vec2(x, y) * shadowTexel).r;
      lit += (p.z - bias > d) ? 0.0 : 1.0;
    }
  return lit / 9.0;
}
void main() {
  vec3 base = toLinear(fragColor.rgb * colDiffuse.rgb);
  if (useTexture == 1) {
    vec4 t = texture(texture0, fragUV);
    if (t.a < 0.5) discard;  // ground raster alpha = real openings (stairwells) in the pavement
    base *= toLinear(t.rgb);
  }
  vec3 n = normalize(fragNormal);
  if (!gl_FrontFacing) n = -n;
  float ndl = max(dot(n, sunDir), 0.0);
  float sh = 1.0;
  if (shadowsOn == 1 && ndl > 0.0) sh = shadowFactor(n);
  float hemi = n.y * 0.5 + 0.5;
  vec3 amb = mix(ambientGround, ambientSky, hemi);
  vec3 col = base * (amb + sunColor * ndl * sh);
  float d = length(viewPos - fragPos);
  float f = 1.0 - exp(-d * fogDensity);
  col = mix(col, toLinear(fogColor), clamp(f, 0.0, 1.0));
  col = aces(col * 0.9);
  finalColor = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}
)";

const char* kDepthVs = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
const char* kDepthFs = R"(#version 330
out vec4 finalColor;
void main() { finalColor = vec4(1.0); }
)";

const char* kSkyVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 uv;
void main() { uv = vertexTexCoord; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
const char* kSkyFs = R"(#version 330
in vec2 uv;
uniform vec2 resolution;
uniform mat4 invViewProj;
uniform vec3 camPos;
uniform vec3 sunDir;
uniform vec3 zenith;
uniform vec3 horizon;
uniform vec3 sunColor;
uniform float sunElev;
out vec4 finalColor;
void main() {
  vec2 ndc = gl_FragCoord.xy / resolution * 2.0 - 1.0;
  vec4 w = invViewProj * vec4(ndc, 1.0, 1.0);
  vec3 dir = normalize(w.xyz / w.w - camPos);
  float h = dir.y;
  vec3 col;
  if (h >= 0.0) col = mix(horizon, zenith, pow(h, 0.45));
  else col = mix(horizon, horizon * 0.55, clamp(-h * 4.0, 0.0, 1.0));
  float sd = max(dot(dir, sunDir), 0.0);
  float vis = smoothstep(-2.0, 2.0, sunElev);
  col += sunColor * (pow(sd, 400.0) * 1.5 + pow(sd, 12.0) * 0.18) * vis;
  if (sd > 0.99996 && h > -0.01) col = mix(col, vec3(1.0, 0.97, 0.9), vis);
  finalColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

Vector3 lerp3(Vector3 a, Vector3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
float smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

}  // namespace

Lighting lightingForSun(float el, float az, float view_distance_m) {
  Lighting L;
  const float e = el * DEG2RAD, a = az * DEG2RAD;
  // ENU direction to the sun -> raylib (x=e, y=u, z=-n)
  const rj::geo::Vec3d enu{std::cos(e) * std::sin(a), std::cos(e) * std::cos(a), std::sin(e)};
  L.sun_dir = Vector3Normalize(enuToRl(enu));
  L.sun_elevation_deg = el;

  const Vector3 day_zenith{0.22f, 0.45f, 0.85f}, day_horizon{0.68f, 0.80f, 0.93f};
  const Vector3 dusk_zenith{0.20f, 0.25f, 0.50f}, dusk_horizon{0.95f, 0.55f, 0.35f};
  const Vector3 night_zenith{0.01f, 0.015f, 0.04f}, night_horizon{0.06f, 0.07f, 0.12f};
  const float day = smooth(2.0f, 18.0f, el);     // 0 at dusk, 1 at day
  const float twi = smooth(-10.0f, 1.0f, el);    // 0 at night, 1 at sunset
  L.sky_zenith = lerp3(lerp3(night_zenith, dusk_zenith, twi), day_zenith, day);
  L.sky_horizon = lerp3(lerp3(night_horizon, dusk_horizon, twi), day_horizon, day);

  const float direct = smooth(-1.0f, 6.0f, el);
  const Vector3 warm{1.0f, 0.62f, 0.38f}, white{1.0f, 0.96f, 0.90f};
  L.sun_color = Vector3Scale(lerp3(warm, white, smooth(3.0f, 25.0f, el)), 1.55f * direct);
  // Ambient: sky light (linear space). Tokyo at night keeps some city glow.
  const Vector3 amb_day{0.36f, 0.40f, 0.48f}, amb_dusk{0.24f, 0.21f, 0.26f}, amb_night{0.035f, 0.04f, 0.06f};
  L.ambient_sky = lerp3(lerp3(amb_night, amb_dusk, twi), amb_day, day);
  L.ambient_ground = Vector3Scale(L.ambient_sky, 0.45f);
  L.fog_density = 0.9f / std::max(300.0f, view_distance_m);
  return L;
}

Lighting indoorLighting() {
  // Artificial light for underground spaces: soft overhead key + bright even ambient.
  Lighting L;
  L.sun_dir = {0.15f, 1.0f, 0.1f};
  L.sun_dir = Vector3Normalize(L.sun_dir);
  L.sun_elevation_deg = -90.0f;  // no sun disc
  L.sun_color = {0.55f, 0.55f, 0.52f};
  L.ambient_sky = {0.72f, 0.73f, 0.72f};
  L.ambient_ground = {0.38f, 0.38f, 0.37f};
  L.sky_zenith = L.sky_horizon = {0.30f, 0.30f, 0.31f};
  L.fog_density = 0.0035f;
  return L;
}

Lighting lerpLighting(const Lighting& a, const Lighting& b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  Lighting L;
  L.sun_dir = Vector3Normalize(Vector3Lerp(a.sun_dir, b.sun_dir, t));
  L.sun_elevation_deg = t < 0.5f ? a.sun_elevation_deg : b.sun_elevation_deg;
  L.sun_color = Vector3Lerp(a.sun_color, b.sun_color, t);
  L.sky_zenith = Vector3Lerp(a.sky_zenith, b.sky_zenith, t);
  L.sky_horizon = Vector3Lerp(a.sky_horizon, b.sky_horizon, t);
  L.ambient_sky = Vector3Lerp(a.ambient_sky, b.ambient_sky, t);
  L.ambient_ground = Vector3Lerp(a.ambient_ground, b.ambient_ground, t);
  L.fog_density = a.fog_density + (b.fog_density - a.fog_density) * t;
  return L;
}

bool Renderer::init() {
  lit_ = LoadShaderFromMemory(kLitVs, kLitFs);
  depth_ = LoadShaderFromMemory(kDepthVs, kDepthFs);
  sky_ = LoadShaderFromMemory(kSkyVs, kSkyFs);
  if (!IsShaderValid(lit_) || !IsShaderValid(depth_) || !IsShaderValid(sky_)) return false;
  lit_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(lit_, "matModel");
  lit_.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(lit_, "matNormal");
  lit_.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(lit_, "viewPos");
  mat_ = LoadMaterialDefault();
  mat_.shader = lit_;
  mat_depth_ = LoadMaterialDefault();
  mat_depth_.shader = depth_;

  shadow_.id = rlLoadFramebuffer();
  shadow_.texture.width = shadow_.texture.height = shadow_res_;
  if (shadow_.id > 0) {
    rlEnableFramebuffer(shadow_.id);
    shadow_.depth.id = rlLoadTextureDepth(shadow_res_, shadow_res_, false);
    shadow_.depth.width = shadow_.depth.height = shadow_res_;
    shadow_.depth.format = 19;
    shadow_.depth.mipmaps = 1;
    rlFramebufferAttach(shadow_.id, shadow_.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(shadow_.id)) TraceLog(LOG_WARNING, "RJ: shadow framebuffer incomplete");
    rlDisableFramebuffer();
  }
  plane_ = GenMeshPlane(12000.0f, 12000.0f, 1, 1);
  body_ = GenMeshCylinder(0.25f, 1.35f, 12);
  legs_ = GenMeshCylinder(0.15f, 0.82f, 8);
  torso_ = GenMeshCylinder(0.21f, 0.64f, 10);
  head_ = GenMeshSphere(0.14f, 10, 12);
  ready_ = true;
  return true;
}

void Renderer::shutdown() {
  if (!ready_) return;
  UnloadMesh(plane_);
  UnloadMesh(body_);
  UnloadMesh(legs_);
  UnloadMesh(torso_);
  UnloadMesh(head_);
  if (shadow_.id) {
    rlUnloadFramebuffer(shadow_.id);
    if (shadow_.depth.id) rlUnloadTexture(shadow_.depth.id);
  }
  mat_.shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
  mat_depth_.shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
  UnloadShader(lit_);
  UnloadShader(depth_);
  UnloadShader(sky_);
  ready_ = false;
}

void Renderer::setClipPlanes(float near_m, float far_m) { rlSetClipPlanes(near_m, far_m); }

void Renderer::renderShadowMap(const Camera3D& cam, const World& world, const Lighting& L) {
  shadow_valid_ = false;
  if (!shadow_.id || L.sun_elevation_deg < 1.0f) return;
  const float extent = 420.0f;
  // Centre the shadow box ahead of the camera and snap to texels to reduce shimmer.
  Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
  fwd.y = 0;
  fwd = Vector3Length(fwd) > 1e-4f ? Vector3Normalize(fwd) : Vector3{0, 0, -1};
  Vector3 center = Vector3Add(cam.position, Vector3Scale(fwd, extent * 0.3f));
  const float snap = extent / static_cast<float>(shadow_res_) * 4.0f;
  center.x = std::round(center.x / snap) * snap;
  center.z = std::round(center.z / snap) * snap;
  center.y = cam.position.y - 20.0f;

  Camera3D lc{};
  lc.position = Vector3Add(center, Vector3Scale(L.sun_dir, 1500.0f));
  lc.target = center;
  lc.up = std::abs(L.sun_dir.y) > 0.99f ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
  lc.projection = CAMERA_ORTHOGRAPHIC;
  lc.fovy = extent;

  BeginTextureMode(shadow_);
  ClearBackground(WHITE);
  rlSetClipPlanes(10.0, 3500.0);
  BeginMode3D(lc);
  const Matrix view = rlGetMatrixModelview();
  const Matrix proj = rlGetMatrixProjection();
  rlDisableBackfaceCulling();
  for (const auto& [code, c] : world.cells())
    for (const auto& m : c->gpu.chunks) DrawMesh(m, mat_depth_, c->model);
  rlEnableBackfaceCulling();
  EndMode3D();
  EndTextureMode();
  light_vp_ = MatrixMultiply(view, proj);
  shadow_valid_ = true;
}

void Renderer::applyLightingUniforms(Shader& s, const Camera3D& cam, const Lighting& L, bool shadows) {
  auto v3 = [&](const char* n, Vector3 v) { SetShaderValue(s, GetShaderLocation(s, n), &v, SHADER_UNIFORM_VEC3); };
  v3("sunDir", L.sun_dir);
  v3("sunColor", L.sun_color);
  v3("ambientSky", L.ambient_sky);
  v3("ambientGround", L.ambient_ground);
  v3("fogColor", L.sky_horizon);
  v3("viewPos", cam.position);
  SetShaderValue(s, GetShaderLocation(s, "fogDensity"), &L.fog_density, SHADER_UNIFORM_FLOAT);
  const int on = (shadows && shadow_valid_) ? 1 : 0;
  SetShaderValue(s, GetShaderLocation(s, "shadowsOn"), &on, SHADER_UNIFORM_INT);
  const float texel = 1.0f / static_cast<float>(shadow_res_);
  SetShaderValue(s, GetShaderLocation(s, "shadowTexel"), &texel, SHADER_UNIFORM_FLOAT);
  SetShaderValueMatrix(s, GetShaderLocation(s, "lightVP"), light_vp_);
}

void Renderer::drawSky(const Camera3D& cam, const Lighting& L, float aspect) {
  const Matrix view = GetCameraMatrix(cam);
  const Matrix proj = MatrixPerspective(cam.fovy * DEG2RAD, aspect, 0.3, 4000.0);
  const Matrix inv = MatrixInvert(MatrixMultiply(view, proj));
  const Vector2 res{static_cast<float>(GetRenderWidth()), static_cast<float>(GetRenderHeight())};
  SetShaderValue(sky_, GetShaderLocation(sky_, "resolution"), &res, SHADER_UNIFORM_VEC2);
  SetShaderValueMatrix(sky_, GetShaderLocation(sky_, "invViewProj"), inv);
  SetShaderValue(sky_, GetShaderLocation(sky_, "camPos"), &cam.position, SHADER_UNIFORM_VEC3);
  SetShaderValue(sky_, GetShaderLocation(sky_, "sunDir"), &L.sun_dir, SHADER_UNIFORM_VEC3);
  SetShaderValue(sky_, GetShaderLocation(sky_, "zenith"), &L.sky_zenith, SHADER_UNIFORM_VEC3);
  SetShaderValue(sky_, GetShaderLocation(sky_, "horizon"), &L.sky_horizon, SHADER_UNIFORM_VEC3);
  const Vector3 sc = Vector3Scale(L.sun_color, 0.4f);
  SetShaderValue(sky_, GetShaderLocation(sky_, "sunColor"), &sc, SHADER_UNIFORM_VEC3);
  SetShaderValue(sky_, GetShaderLocation(sky_, "sunElev"), &L.sun_elevation_deg, SHADER_UNIFORM_FLOAT);
  BeginShaderMode(sky_);
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
  EndShaderMode();
}

void Renderer::drawWorld(const Camera3D& cam, const World& world, const Lighting& L, bool shadows, bool photo_textures,
                         bool neutral_floor) {
  applyLightingUniforms(lit_, cam, L, shadows);
  const int slot = 10;
  rlEnableShader(lit_.id);
  rlActiveTextureSlot(slot);
  rlEnableTexture(shadow_valid_ ? shadow_.depth.id : rlGetTextureIdDefault());
  rlSetUniform(GetShaderLocation(lit_, "shadowMap"), &slot, SHADER_UNIFORM_INT, 1);
  rlActiveTextureSlot(0);

  rlDisableBackfaceCulling();
  int use_tex = 1;
  for (const auto& [code, c] : world.cells()) {
    if (!c->gpu.terrain.vaoId) continue;
    SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
    mat_.maps[MATERIAL_MAP_DIFFUSE].texture = c->gpu.ground;
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    DrawMesh(c->gpu.terrain, mat_, c->model);
  }
  // Photo-textured building faces (PLATEAU appearance atlases). With photo textures
  // disabled (they contain real signage / advertising) the same faces are drawn plain.
  use_tex = photo_textures ? 1 : 0;
  SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
  if (!photo_textures) mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{196, 194, 188, 255};
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.chunks.size(); ++i) {
      const int page = c->gpu.chunk_page[i];
      if (page < 0 || page >= static_cast<int>(c->gpu.pages.size()) || !c->gpu.pages[static_cast<size_t>(page)].id) continue;
      mat_.maps[MATERIAL_MAP_DIFFUSE].texture =
          photo_textures ? c->gpu.pages[static_cast<size_t>(page)] : Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
      DrawMesh(c->gpu.chunks[i], mat_, c->model);
    }
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  use_tex = 0;
  SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  if (auto mz = world.minTerrainZ(); mz && neutral_floor) {
    // Neutral floor outside the data coverage (no invented content), below all real terrain.
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{92, 92, 90, 255};
    DrawMesh(plane_, mat_, MatrixTranslate(0.0f, static_cast<float>(*mz) - 1.5f, 0.0f));
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  }
  for (const auto& [code, c] : world.cells())
    for (size_t i = 0; i < c->gpu.chunks.size(); ++i) {
      const int page = c->gpu.chunk_page[i];
      if (page >= 0 && page < static_cast<int>(c->gpu.pages.size()) && c->gpu.pages[static_cast<size_t>(page)].id) continue;
      DrawMesh(c->gpu.chunks[i], mat_, c->model);
    }
  rlEnableBackfaceCulling();
}

void Renderer::drawPlayerBody(const Vector3& feet, float yaw_rad, const Lighting& L) {
  (void)L;
  int use_tex = 0;
  SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{40, 70, 140, 255};
  const Matrix body = MatrixMultiply(MatrixRotateY(-yaw_rad), MatrixTranslate(feet.x, feet.y + 0.05f, feet.z));
  DrawMesh(body_, mat_, body);
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{230, 200, 170, 255};
  DrawMesh(head_, mat_, MatrixTranslate(feet.x, feet.y + 1.58f, feet.z));
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
}

void Renderer::drawInterior(const Interior& in, const Camera3D& cam, const Lighting& L, bool shadows) {
  applyLightingUniforms(lit_, cam, L, shadows);
  int use_tex = 0;
  SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
  mat_.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, 7};
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
  rlDisableBackfaceCulling();
  for (const auto& m : in.meshes()) DrawMesh(m, mat_, in.model());
  rlEnableBackfaceCulling();
}

void Renderer::drawPedestrians(const Pedestrians& peds) {
  int use_tex = 0;
  SetShaderValue(lit_, GetShaderLocation(lit_, "useTexture"), &use_tex, SHADER_UNIFORM_INT);
  for (const auto& [id, w] : peds.walkers()) {
    const float s = w.height_scale;
    const float bob = std::fabs(std::sin(w.phase)) * 0.035f * s;
    const Vector3 feet = enuToRl({w.pos.x, w.pos.y, static_cast<double>(w.z)});
    const float yaw = std::atan2(static_cast<float>(w.dir.x), static_cast<float>(w.dir.y));  // compass heading
    const Matrix rot = MatrixRotateY(-yaw);
    auto at = [&](float y) { return MatrixMultiply(MatrixMultiply(MatrixScale(s, s, s), rot), MatrixTranslate(feet.x, feet.y + y, feet.z)); };
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = w.pants;
    DrawMesh(legs_, mat_, at(bob));
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = w.shirt;
    DrawMesh(torso_, mat_, at(0.80f * s + bob));
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = w.skin;
    DrawMesh(head_, mat_, MatrixMultiply(MatrixScale(s, s, s), MatrixTranslate(feet.x, feet.y + 1.58f * s + bob, feet.z)));
  }
  mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
}

}  // namespace rjc
