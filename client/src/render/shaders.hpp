#pragma once
// GLSL 3.30 sources for the forward PBR renderer and the post-processing chain.

namespace rjc::shaders {

// ---------------------------------------------------------------------------
// Lit (PBR) — every opaque surface in the world.
inline const char* kLitVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 matView;
uniform mat4 matProjection;
uniform float timeSec;
uniform float windStrength;
out vec3 fragPos;
out vec3 fragNormal;
out vec4 fragColor;
out vec2 fragUV;
out vec2 fragMat;
void main() {
  vec4 wp = matModel * vec4(vertexPosition, 1.0);
  if (abs(vertexTexCoord2.x - 33.0) < 0.5 && vertexTexCoord2.y > 0.0) {
    // foliage sways with the wind (tips more than the inner crown)
    float ph = timeSec * 1.7 + wp.x * 0.21 + wp.z * 0.17;
    float a = vertexTexCoord2.y * windStrength;
    wp.xz += vec2(sin(ph), cos(ph * 0.83)) * 0.07 * a + vec2(sin(ph * 3.1), sin(ph * 2.7)) * 0.02 * a;
  }
  fragPos = wp.xyz;
  fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
  fragColor = vertexColor;
  fragUV = vertexTexCoord;
  fragMat = vertexTexCoord2;
  gl_Position = matProjection * matView * wp;
}
)";

// Shared lighting/atmosphere code (prepended to lit + sky fragment shaders).
#define RJ_COMMON_GLSL R"(
const float PI = 3.14159265;
uniform vec3 sunDir;       // towards the sun (raylib space, y up)
uniform vec3 sunColor;     // linear irradiance (already includes atmospheric extinction)
uniform vec3 skyZenith;
uniform vec3 skyHorizon;
uniform vec3 ambientSky;
uniform vec3 ambientGround;
uniform vec3 hazeColor;
uniform float exposure;
uniform float cloudCover;  // 0 clear .. 1 overcast
uniform vec2 cloudOffset;  // wind drift (m)
uniform sampler2D texNoise;
vec3 aces(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toneMap(vec3 c) { return pow(aces(c * exposure), vec3(1.0 / 2.2)); }
// Sky radiance in direction d (no sun disc); used for the sky, reflections and aerial perspective.
vec3 skyRadiance(vec3 d) {
  float h = d.y;
  float mu = max(dot(d, sunDir), 0.0);
  vec3 c;
  if (h >= 0.0) {
    c = mix(skyHorizon, skyZenith, pow(h, 0.42));
    c = mix(c, hazeColor, exp(-h * 14.0) * 0.55);  // humid horizon band (Tokyo haze)
  } else {
    c = mix(hazeColor * 0.85, ambientGround * 1.4, clamp(-h * 6.0, 0.0, 1.0));  // city below the horizon
  }
  c += sunColor * (0.020 * pow(mu, 6.0) + 0.06 * pow(mu, 48.0)) * (1.0 - cloudCover * 0.6);  // Mie forward glow
  return mix(c, mix(skyHorizon, ambientSky * 1.3, 0.5) * (0.75 + 0.25 * max(h, 0.0)), cloudCover * 0.85);
}
float cloudDensityLod(vec2 p, float far) {
  // p in metres on the cloud layer; far (0..1) removes fine octaves near the horizon
  vec2 q = (p + cloudOffset) * 0.00014;
  vec2 warp = vec2(texture(texNoise, q * 0.7 + 0.13).g, texture(texNoise, q * 0.7 + 0.61).r) - 0.5;
  q += warp * 0.35;
  float n = texture(texNoise, q).r * 0.6 + texture(texNoise, q * 2.3 + 0.37).g * 0.28 * (1.0 - far * 0.7)
          + texture(texNoise, q * 5.1 + 0.71).b * 0.12 * (1.0 - far);
  float cov = mix(0.58, 0.18, cloudCover);
  return smoothstep(cov, cov + 0.28, n);
}
float cloudDensityAt(vec2 p) { return cloudDensityLod(p, 0.0); }
)"

inline const char* kLitFs = "#version 330\n" RJ_COMMON_GLSL R"(
in vec3 fragPos;
in vec3 fragNormal;
in vec4 fragColor;
in vec2 fragUV;
in vec2 fragMat;
uniform sampler2D texture0;   // ground raster / photo atlas / white
uniform sampler2D texAO;      // ground contact occlusion (terrain)
uniform sampler2D texAsphalt; // rgb albedo tint, a roughness
uniform sampler2D texAsphaltN;// rg normal xy, b height, a cavity
uniform sampler2D texPaving;
uniform sampler2D texPavingN;
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform mat4 lightVP0;
uniform mat4 lightVP1;
uniform float shadowTexel0;
uniform float shadowTexel1;
uniform int shadowsOn;
uniform vec4 colDiffuse;
uniform int useTexture;
uniform int surfaceMode;      // 0 mesh, 1 terrain ground raster, 2 photo facade
uniform int materialOverride; // >= 0 forces a material id for the whole draw
uniform vec3 viewPos;
uniform float fogDensity;
uniform float wetness;
uniform float nightFactor;    // 0 day .. 1 night (artificial lights on)
uniform float timeSec;
uniform float indoor;         // 1 = underground interior (no sky reflection)
uniform vec3 selfLight;       // per-draw interior lighting (ceiling lights of a train / cabin interior)
uniform int numLights;
uniform vec4 lightPosR[32];   // xyz, range (Renderer::kMaxLights)
uniform vec3 lightCol[32];
uniform vec3 emissiveTint;    // per-draw emission (signal lamps)
uniform vec3 occupancy;       // fraction of lit windows: office, residential, shop (by time of day)
uniform vec3 partTop;         // per-person colours (sRGB 0..1): clothing top / bottom, skin, hair
uniform vec3 partBottom;
uniform vec3 partSkin;
uniform vec3 partHair;
// Fictional country: land cover of the terrain being drawn, snow potential of the whole country,
// the season (snow, rice paddies, leaves) - see Renderer::setSeason
uniform sampler2D texLand;    // RGBA weights: forest, paddy, field, bare (terrain draws, same UV as the ground)
uniform int landOn;
uniform sampler2D texSnow;    // snow potential over the country (0..1)
uniform vec3 snowU;           // snow-map u = dot(vec3(x, z, 1), snowU) (raylib x, z)
uniform vec3 snowV;
uniform float snowSeason;     // 0 no snow lying .. 1 deep winter
uniform float cropStage;      // 0 winter stubble, 1 flooded with seedlings, 2 green, 3 golden
uniform float canopyCut;      // > 0: the canopy mesh gives way to single trees within this radius of the camera
uniform float leafStage;      // 0 green, 1 autumn colours, 2 bare (deciduous trees)
out vec4 finalColor;
float litFrom(float p) {
  float cat = floor(p);
  float occ = cat < 0.5 ? occupancy.x : (cat < 1.5 ? occupancy.y : occupancy.z);
  return step(fract(p), occ);
}

float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }

float shadowPCF(sampler2D sm, vec3 p, float texel, float bias) {
  float lit = 0.0;
  for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++) {
      float d = texture(sm, p.xy + vec2(x, y) * texel).r;
      lit += (p.z - bias > d) ? 0.0 : 1.0;
    }
  return lit / 9.0;
}
float sunShadow(vec3 n) {
  if (shadowsOn == 0) return 1.0;
  vec4 a = lightVP0 * vec4(fragPos + n * 0.03, 1.0);
  vec3 p = a.xyz / a.w * 0.5 + 0.5;
  if (p.x > 0.02 && p.x < 0.98 && p.y > 0.02 && p.y < 0.98 && p.z < 1.0) return shadowPCF(shadowMap0, p, shadowTexel0, 0.00012);
  vec4 b = lightVP1 * vec4(fragPos + n * 0.12, 1.0);
  vec3 q = b.xyz / b.w * 0.5 + 0.5;
  if (q.x <= 0.0 || q.x >= 1.0 || q.y <= 0.0 || q.y >= 1.0 || q.z >= 1.0) return 1.0;
  return shadowPCF(shadowMap1, q, shadowTexel1, 0.00022);
}

struct Surf { vec3 albedo; float rough; float metal; vec3 n; float ao; vec3 emit; float porosity; vec3 transmit; };

vec3 tangentNormal(vec3 ng, vec2 nxy, float strength) {
  vec3 T = vec3(1.0, 0.0, 0.0), B = vec3(0.0, 0.0, -1.0);  // world east / north for planar UVs
  vec3 t = vec3((nxy * 2.0 - 1.0) * strength, 1.0);
  return normalize(T * t.x + B * t.y + ng * t.z);
}

// Procedural manhole lid (UV 0..1 across the lid).
void manhole(inout Surf s, vec2 uv) {
  vec2 c = uv - 0.5;
  float r = length(c) * 2.0;
  float rim = smoothstep(0.86, 0.9, r) - smoothstep(0.96, 1.0, r);
  float ang = atan(c.y, c.x);
  float pat = step(0.5, fract(r * 9.0)) * step(0.5, fract(ang * 6.0 / PI + r * 3.0));
  s.albedo = vec3(0.10, 0.095, 0.09) * (0.8 + 0.4 * pat) + vec3(0.03) * rim;
  s.metal = 0.75;
  s.rough = 0.42 + 0.25 * pat;
  s.n = normalize(s.n + vec3(0.0, 0.0, 0.0));
}

// Interior mapping: the view ray continues behind the glass and hits the room's back wall,
// ceiling or floor (heights from the vertex UV: x = floor, y = ceiling, raylib space).
vec3 interiorBehind(vec3 ng, bool shop, float lit) {
  vec3 V = normalize(viewPos - fragPos);
  vec3 d = -V;
  vec3 inward = -ng;
  float floorY = fragUV.x, ceilY = fragUV.y;
  if (ceilY <= floorY + 0.5) { floorY = fragPos.y - 1.0; ceilY = fragPos.y + 1.6; }
  float depth = shop ? 5.0 : 4.0;
  float dn = max(dot(d, inward), 0.05);
  float tBack = depth / dn;
  float tCeil = d.y > 1e-4 ? (ceilY - fragPos.y) / d.y : 1e9;
  float tFloor = d.y < -1e-4 ? (floorY - fragPos.y) / d.y : 1e9;
  float t = min(tBack, min(tCeil, tFloor));
  vec3 p = fragPos + d * t;
  vec3 tang = normalize(cross(ng, vec3(0.0, 1.0, 0.0)));
  float along = dot(p, tang);
  float into = dot(p - fragPos, inward);
  float room = hash12(floor(vec2(along / 3.3, floorY / 3.1)));
  if (t == tCeil) {
    // ceiling: soft pools of light (no hard stripes: they read as artefacts in perspective)
    float spot = smoothstep(0.55, 0.0, length(fract(vec2(along * 0.4, into * 0.4)) - 0.5));
    return vec3(0.62, 0.61, 0.58) * (0.2 + 0.8 * lit) + vec3(1.0, 0.97, 0.92) * spot * lit * (shop ? 1.6 : 1.0);
  }
  if (t == tFloor) {
    float fade = exp(-into * 0.25);
    return (shop ? vec3(0.40, 0.38, 0.35) : vec3(0.30, 0.24, 0.18)) * (0.25 + 0.75 * lit) * (0.6 + 0.4 * fade);
  }
  // back wall: shelves of goods (shop) or a room wall with furniture / curtains
  float h = p.y - floorY;
  if (shop) {
    float shelf = smoothstep(0.02, 0.0, abs(fract(h * 1.8) - 0.9));
    float item = hash12(floor(vec2(along * 1.2, h * 1.8)));
    vec3 goods = mix(vec3(0.58, 0.55, 0.50), vec3(0.95, 0.85, 0.7) * (0.55 + 0.45 * item), step(h, 2.0) * 0.5);
    return goods * (1.0 - 0.35 * shelf) * (0.3 + 0.7 * lit);
  }
  vec3 wall = mix(vec3(0.58, 0.54, 0.48), vec3(0.74, 0.72, 0.67), room);
  float curtain = step(0.6, room);
  return mix(wall, vec3(0.86, 0.84, 0.77), curtain * 0.7) * (0.2 + 0.8 * lit);
}

// Lying snow at this point (0..1): the country's snow potential times the season, on surfaces that
// face upwards, patchy at the margins.
float snowAt(vec3 p, vec3 n) {
  if (snowSeason <= 0.001) return 0.0;
  vec2 uv = vec2(dot(vec3(p.x, p.z, 1.0), snowU), dot(vec3(p.x, p.z, 1.0), snowV));
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  float pot = texture(texSnow, uv).r;
  float patchy = texture(texNoise, p.xz / 37.0).r * 0.35 + texture(texNoise, p.xz / 5.3).g * 0.15;
  float s = smoothstep(0.30, 0.62, pot * snowSeason * 1.25 + patchy - 0.25);
  return s * smoothstep(0.35, 0.75, n.y);
}
vec3 snowAlbedo(vec2 wuv) { return vec3(0.80, 0.83, 0.88) * (0.92 + 0.08 * texture(texNoise, wuv / 1.7).b); }

Surf material(int id, vec3 ng, vec2 wuv) {
  Surf s;
  vec3 vc = pow(fragColor.rgb * colDiffuse.rgb, vec3(2.2));
  s.albedo = vc; s.rough = 0.8; s.metal = 0.0; s.n = ng; s.ao = 1.0; s.emit = vec3(0.0); s.porosity = 0.5; s.transmit = vec3(0.0);
  vec4 nz = texture(texNoise, wuv * 0.25);
  if (id == 1) {            // curb stone
    s.albedo = vec3(0.42, 0.41, 0.39) * (0.85 + 0.3 * nz.b);
    s.rough = 0.85; s.porosity = 0.6;
  } else if (id == 2 || id == 15) {  // sidewalk paving blocks / traffic island
    vec2 puv = wuv / (id == 2 ? 1.2 : 0.9);
    vec4 A = texture(texPaving, puv), N = texture(texPavingN, puv);
    s.albedo = A.rgb * (id == 2 ? 1.0 : 0.85);
    s.rough = A.a; s.n = tangentNormal(ng, N.rg, 0.6); s.ao = mix(1.0, N.a, 0.8); s.porosity = 0.7;
    float sn = snowAt(fragPos, ng) * 0.8;
    s.albedo = mix(s.albedo, snowAlbedo(wuv), sn); s.rough = mix(s.rough, 0.6, sn);
  } else if (id == 3) {     // tactile paving (yellow, raised dots)
    vec2 g = fract(wuv / 0.3) - 0.5;
    vec2 d = fract(wuv / 0.06) - 0.5;
    float dot_ = 1.0 - smoothstep(0.18, 0.24, length(d));
    float joint = step(0.47, max(abs(g.x), abs(g.y)));
    s.albedo = mix(vec3(0.62, 0.43, 0.03), vec3(0.2), joint * 0.6) * (0.9 + 0.2 * nz.b);
    s.n = normalize(ng + vec3(d.x, 0.0, -d.y) * dot_ * 1.2);
    s.rough = 0.55; s.porosity = 0.2;
  } else if (id == 4) {     // road-marking paint (worn)
    float wear = smoothstep(0.35, 0.75, texture(texNoise, wuv * 0.6).b);
    s.albedo = mix(vec3(0.78, 0.78, 0.74), vec3(0.2, 0.2, 0.21), wear * 0.45);
    s.rough = 0.55; s.porosity = 0.25;
  } else if (id == 5) { s.rough = 0.42; s.metal = 0.55; s.porosity = 0.0; }
  else if (id == 6) { s.rough = 0.5; s.metal = 0.3; s.porosity = 0.0; }
  else if (id == 7) { s.rough = 0.35; s.metal = 0.1; s.porosity = 0.0; }
  else if (id == 8) { manhole(s, fragUV); s.porosity = 0.0; }
  else if (id == 9) {       // gutter grating
    float bar = step(0.55, fract(dot(wuv, vec2(1.0)) / 0.04));
    s.albedo = mix(vec3(0.03), vec3(0.14, 0.13, 0.12), bar); s.metal = 0.7; s.rough = 0.5; s.porosity = 0.0;
  } else if (id == 10) {    // street-light lamp (emissive at night)
    s.albedo = vec3(0.8, 0.8, 0.75); s.rough = 0.2;
    s.emit = vec3(1.0, 0.86, 0.66) * 14.0 * nightFactor;
  } else if (id == 11 || id == 20 || id == 21) {  // glass (windows, shop glass)
    s.albedo = vec3(0.015, 0.02, 0.025); s.rough = 0.04; s.metal = 0.0; s.porosity = 0.0;
    float lit = id == 11 ? 0.0 : litFrom(fragMat.y);
    if (id == 20) {
      // By day interiors are much darker than the street; lit rooms glow at night.
      float b = mix(0.06, 0.38, nightFactor) * (0.4 + 0.9 * fract(fragMat.y * 7.13));  // rooms differ
      vec3 tint = fragMat.y < 1.0 ? vec3(0.88, 0.94, 1.0) : vec3(1.0, 0.84, 0.62);  // office LED vs home
      s.transmit = interiorBehind(ng, false, lit) * tint * b;
    }
    if (id == 21) {
      float b = mix(0.28, 0.8, nightFactor);
      s.transmit = interiorBehind(ng, true, lit) * b;
    }
  } else if (id == 12) { s.albedo *= 0.85 + 0.3 * nz.b; s.rough = 0.9; s.porosity = 0.6; }
  else if (id == 13) { s.rough = 0.38; s.metal = 0.15; s.porosity = 0.0; }
  else if (id == 14) { s.albedo = vec3(0.30, 0.20, 0.11); s.metal = 1.0; s.rough = 0.38; s.porosity = 0.0; }
  else if (id == 22) { s.rough = 0.32; s.metal = 0.85; s.porosity = 0.0; }             // aluminium frames
  else if (id == 23) { s.rough = 0.9; s.porosity = 0.3; }                               // awning fabric
  else if (id == 24) {  // back-lit sign band, lettered with generic glyph blocks (no real names)
    s.rough = 0.4;
    float ink = 0.0;
    if (abs(ng.y) < 0.3) {
      vec3 hn = normalize(vec3(ng.x, 0.0, ng.z));
      float lx = dot(fragPos, vec3(-hn.z, 0.0, hn.x)) - fragUV.y;  // m from the band centre, along
      float ly = fragPos.y - fragUV.x;                               // m from the band centre, up
      float halfw = fragColor.a * 25.5;
      float seed = hash12(vec2(floor(fragUV.y * 3.1), floor(fragUV.x * 7.3)));
      float cw = 0.46, chh = 0.44;
      float n = floor(mix(2.0, max(2.0, min(8.0, (halfw * 2.0 - 1.2) / cw)), seed));
      float x0 = -n * cw * 0.5 + (fract(seed * 5.7) - 0.5) * max(0.0, halfw - n * cw * 0.5 - 0.7);
      float cx = (lx - x0) / cw;
      if (cx >= 0.0 && cx < n && abs(ly) < chh * 0.5) {
        float ci = floor(cx);
        vec2 g = vec2(fract(cx), ly / chh + 0.5);
        vec2 q = floor(g * 3.0), r = fract(g * 3.0);
        float hs = step(0.45, hash12(vec2(ci, q.y) + seed * 13.0)) * step(abs(r.y - 0.5), 0.2);
        float vs = step(0.55, hash12(vec2(ci + 7.0, q.x) + seed * 29.0)) * step(abs(r.x - 0.5), 0.2);
        ink = max(hs, vs) * step(0.1, g.x) * step(g.x, 0.9) * step(0.06, g.y) * step(g.y, 0.94);
      }
      // round logo mark before the lettering
      float lm = length(vec2(lx - (x0 - 0.42), ly));
      ink = max(ink, step(lm, 0.25) * step(0.5, fract(seed * 11.3)));
    }
    vec3 base = fragColor.rgb * colDiffuse.rgb;
    vec3 inkCol = dot(base, vec3(0.3, 0.59, 0.11)) > 0.5 ? vec3(0.1) : vec3(0.96);
    vec3 face = mix(base, inkCol, ink);
    s.albedo = pow(face, vec3(2.2));
    s.emit = pow(face, vec3(2.2)) * 1.7 * nightFactor * litFrom(fragMat.y);
  }
  else if (id == 25) { s.rough = 0.55; s.metal = 0.2; s.porosity = 0.0; }             // AC outdoor unit
  else if (id == 26) { s.rough = 0.6; s.metal = 0.4; s.porosity = 0.0; }              // balcony rail
  else if (id == 27) { s.albedo *= 0.9 + 0.2 * nz.b; s.rough = 0.85; }                // wall paint
  else if (id == 28) { s.rough = 0.22; s.metal = 0.45; s.porosity = 0.0; }            // car paint
  else if (id == 29) { s.albedo = vec3(0.02); s.rough = 0.9; }                        // tyre
  else if (id == 30) { s.albedo = vec3(0.02); s.rough = 0.25; s.emit = emissiveTint; } // signal lamp lens
  else if (id == 31) { s.albedo = pow(partTop, vec3(2.2)) * fragColor.r; s.rough = 0.9; s.porosity = 0.8; }     // clothing top
  else if (id == 36) { s.albedo = pow(partBottom, vec3(2.2)) * fragColor.r; s.rough = 0.88; s.porosity = 0.8; }  // bottoms
  else if (id == 32) { s.albedo = pow(partSkin, vec3(2.2)) * fragColor.r; s.rough = 0.5; }                        // skin
  else if (id == 37) { s.albedo = pow(partHair, vec3(2.2)) * fragColor.r; s.rough = 0.42; }                       // hair
  else if (id == 33) { s.rough = 0.7; s.porosity = 0.1; }                           // leaves
  else if (id == 38) {  // train windows / interior lights: dark glass, lit inside while in service
    s.albedo = vec3(0.02, 0.025, 0.03); s.rough = 0.12; s.porosity = 0.0;
    s.emit = vec3(0.85, 0.92, 1.0) * (0.04 + 0.7 * nightFactor);
    vec3 ec = fragColor.rgb;
    if (max(ec.r, max(ec.g, ec.b)) - min(ec.r, min(ec.g, ec.b)) > 0.08) s.emit = pow(ec, vec3(2.2)) * 1.6;  // coloured LED displays: always lit
    else if (ec.r < 0.5) s.emit *= 0.3;                                                                    // dark panes (door windows)
  }
  else if (id == 16) {  // water (river / sea): dark, glossy, moving ripples
    vec2 w1 = texture(texNoise, wuv / 23.0 + vec2(timeSec * 0.011, timeSec * 0.007)).rg;
    vec2 w2 = texture(texNoise, wuv / 7.0 - vec2(timeSec * 0.017, -timeSec * 0.013)).gb;
    vec2 wv = (w1 + w2 - 1.0) * 0.16;
    s.n = normalize(ng + vec3(wv.x, 0.0, wv.y));
    s.albedo = vec3(0.012, 0.03, 0.036); s.rough = 0.035; s.porosity = 0.0;
  }
  else if (id == 17) {  // forest canopy seen from afar: clumpy crowns
    vec4 c = texture(texNoise, wuv / 6.0);
    // stands of different species and age (cedar blue-green, broadleaf yellow-green) at 40-150 m; from
    // afar, where single crowns are below a pixel, the shade between them darkens the canopy
    vec4 cs = texture(texNoise, wuv / 41.0), cl = texture(texNoise, wuv / 150.0);
    float far = smoothstep(0.15, 1.2, length(fwidth(wuv)));
    vec3 green = mix(vec3(0.032, 0.062, 0.034), vec3(0.058, 0.085, 0.028), smoothstep(0.3, 0.7, cl.r))
                 * (0.6 + 0.8 * mix(c.r, 0.5, far)) * (0.8 + 0.4 * cs.g) * mix(1.0, 0.82, far);
    // mixed forest: evergreen conifers (cedar / cypress plantations) and broadleaf stands in patches;
    // the broadleaf trees turn red and yellow in autumn and are bare in winter
    // (single tree crowns flag broadleaf / conifer in the vertex colour's blue channel)
    float broad = fragColor.b > 0.95 ? 1.0 : fragColor.b < 0.05 ? 0.0 : smoothstep(0.45, 0.65, texture(texNoise, wuv / 180.0).g);
    float hueN = texture(texNoise, wuv / 23.0).b;
    vec3 autumn = mix(vec3(0.30, 0.06, 0.02), vec3(0.34, 0.20, 0.03), hueN);
    vec3 bare = vec3(0.07, 0.055, 0.045) * (0.7 + 0.6 * c.r);
    vec3 leaf = leafStage < 1.0 ? mix(green, autumn, leafStage) : mix(autumn, bare, leafStage - 1.0);
    s.albedo = mix(green, leaf, broad) * (fragColor.r * 2.0);
    s.n = normalize(ng + vec3(c.g - 0.5, 0.0, c.b - 0.5) * 0.9);
    s.rough = 0.8; s.porosity = 0.2;
    float sn = snowAt(fragPos, ng) * (0.55 + 0.45 * c.r);
    s.albedo = mix(s.albedo, snowAlbedo(wuv), sn * 0.85);
  }
  else if (id == 18) {  // asphalt deck (bridges)
    vec4 A = texture(texAsphalt, wuv / 3.5);
    s.albedo = vec3(0.15) * A.rgb; s.rough = A.a; s.porosity = 0.8;
  }
  else if (id == 19) {  // ballast + sleepers on the elevated tracks
    float sl = step(0.62, fract(wuv.x / 0.6 + wuv.y / 0.6));
    s.albedo = mix(vec3(0.18, 0.17, 0.16) * (0.7 + 0.6 * nz.r), vec3(0.22, 0.2, 0.18), sl * 0.6);
    s.rough = 0.9; s.porosity = 0.5;
  }
  else if (id == 41) {  // far-view building box: plain walls, lit windows at night, snow on the roof
    s.rough = 0.85;
    if (abs(ng.y) < 0.5) {
      vec2 cell = floor(vec2(fragPos.x + fragPos.z, fragPos.y) / vec2(3.2, 3.2));
      float lit = step(hash12(cell), mix(occupancy.y, occupancy.x, 0.5) * 0.85);
      float win = step(0.25, fract((fragPos.x + fragPos.z) / 3.2)) * step(0.3, fract(fragPos.y / 3.2)) * step(0.99, fragColor.a);
      s.albedo *= mix(1.0, 0.55, win * 0.6);
      s.emit = vec3(1.0, 0.86, 0.62) * lit * win * nightFactor * 0.9;
    } else {
      s.albedo = mix(s.albedo, snowAlbedo(wuv), snowAt(fragPos, ng));
    }
  }
  else if (id == 34) { s.albedo *= 0.8 + 0.3 * nz.b; s.rough = 0.92; s.porosity = 0.5; } // bark
  else if (id == 35) { s.albedo = pow(fragColor.rgb, vec3(2.2)); s.rough = 0.45; s.porosity = 0.0; }  // untinted
  return s;
}

// ---- Procedural facades (fictional island buildings, RJCELL page -2) ----------------------
// uv = facade metres / (2048, 1024): x along the facade (1024 = its centre), y above the building's
// ground; roofs: plan / slope metres. Vertex alpha = style code (pipeline/island/buildings.py).
float aaBand(float lo, float hi, float x, float w) { return smoothstep(lo - w, lo + w, x) - smoothstep(hi - w, hi + w, x); }
float rect2(vec2 p, vec2 lo, vec2 hi, vec2 w) { return aaBand(lo.x, hi.x, p.x, w.x) * aaBand(lo.y, hi.y, p.y, w.y); }
// Generic lettering: a 3x3 stroke grid per character (kanji / katakana-like blocks, no real text).
float glyphInk(vec2 g, float ci, float seed) {
  vec2 q = floor(g * 3.0), r = fract(g * 3.0);
  float hs = step(0.45, hash12(vec2(ci, q.y) + seed * 13.0)) * step(abs(r.y - 0.5), 0.2);
  float vs = step(0.55, hash12(vec2(ci + 7.0, q.x) + seed * 29.0)) * step(abs(r.x - 0.5), 0.2);
  return max(hs, vs) * step(0.1, g.x) * step(g.x, 0.9) * step(0.08, g.y) * step(g.y, 0.92);
}
vec3 hue(float h) { return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0); }

Surf facade(vec3 ng) {
  Surf s;
  int st = int(fragColor.a * 255.0 + 0.5);
  vec3 base = fragColor.rgb;
  vec3 wall = pow(base, vec3(2.2));
  vec2 m = fragUV * vec2(2048.0, 1024.0);
  vec2 fw = max(fwidth(m), vec2(1e-4));
  float seed = hash12(floor(base.rg * 255.0) + floor(base.b * 255.0) * 0.37);
  s.albedo = wall; s.rough = 0.85; s.metal = 0.0; s.n = ng; s.ao = 1.0; s.emit = vec3(0.0); s.porosity = 0.4;
  s.transmit = vec3(0.0);
  vec3 T = normalize(cross(vec3(0.0, 1.0, 0.0), ng) + vec3(1e-5, 0.0, 0.0));
  float night = nightFactor;
  vec4 nz = texture(texNoise, m * 0.07);
  if (st >= 100 && st < 200) {  // ---- roofs ----
    if (st == 101) {  // kawara tiles: rows down the slope, round channels across
      float row = fract(m.y / 0.26);
      float ch = sin(m.x * 6.2832 / 0.3);
      float tv = hash12(floor(vec2(m.x / 0.3, m.y / 0.26)));
      s.albedo = wall * (0.78 + 0.3 * tv) * (1.0 - 0.35 * smoothstep(0.82, 1.0, row));
      vec3 B = normalize(cross(ng, T));
      s.n = normalize(ng + T * ch * 0.25 * (1.0 - smoothstep(0.3, 1.2, fw.x * 8.0)));
      s.rough = 0.5; s.porosity = 0.15;
    } else if (st == 102) {  // standing-seam metal
      float seam = 1.0 - aaBand(0.04, 0.96, fract(m.x / 0.45), fw.x / 0.45);
      s.albedo = wall * (0.9 + 0.1 * nz.r) * (1.0 + 0.25 * seam);
      s.metal = 0.35; s.rough = 0.42; s.porosity = 0.0;
    } else {  // flat concrete roof with joints and stains
      vec2 j = abs(fract(m / 3.0) - 0.5);
      float joint = 1.0 - smoothstep(0.47, 0.49, max(j.x, j.y));
      s.albedo = wall * (0.72 + 0.4 * nz.b) * (1.0 - 0.25 * (1.0 - joint));
      s.rough = 0.9; s.porosity = 0.6;
    }
    // snow lies on the roofs in the snow country (steep metal roofs shed some of it)
    float sn = snowAt(fragPos, ng) * (st == 102 ? 0.8 : 1.0);
    s.albedo = mix(s.albedo, vec3(0.80, 0.83, 0.88), sn); s.rough = mix(s.rough, 0.6, sn); s.metal *= 1.0 - sn;
    return s;
  }
  if (st >= 200) {  // ---- special surfaces ----
    float lx = m.x - 1024.0;
    if (st == 200) {  // vertical neon sign: characters stacked downwards
      float ch = 0.72;
      float ci = floor((m.y - 0.3) / ch);
      vec2 g = vec2(lx / 0.9 + 0.5, fract((m.y - 0.3) / ch));
      float ink = glyphInk(vec2(g.x, 1.0 - g.y), ci, seed) * step(0.3, m.y);
      float border = 1.0 - aaBand(0.05, 0.95, lx / 0.9 + 0.5, fw.x);
      vec3 inkCol = dot(base, vec3(0.3, 0.59, 0.11)) > 0.62 ? vec3(0.12) : vec3(1.0);
      vec3 face = mix(mix(base, inkCol, ink), vec3(0.95), border * 0.6);
      s.albedo = pow(face, vec3(2.2)) * 0.8; s.rough = 0.3; s.porosity = 0.0;
      s.emit = pow(face, vec3(2.2)) * (0.35 + 3.4 * night);
    } else if (st == 201) {  // LED screen: abstract animated content (never real advertising)
      float scene = floor(timeSec / 7.0 + seed * 5.0);
      float sh = hash12(vec2(scene, seed));
      vec2 p = vec2(lx, m.y);
      vec3 c;
      if (sh < 0.33) c = hue(fract(p.x * 0.03 - timeSec * 0.1 + sh)) * (0.6 + 0.4 * sin(p.y * 0.4 + timeSec * 2.0));
      else if (sh < 0.66) {
        vec2 cell = floor(p / 2.4);
        c = hue(hash12(cell + scene)) * step(0.35, hash12(cell * 1.7 + floor(timeSec * 1.5)));
      } else {
        float ci = floor(p.x / 2.0);
        float ink = glyphInk(vec2(fract(p.x / 2.0), fract(p.y / 2.4)), ci + scene * 11.0, sh);
        c = mix(hue(sh + 0.1) * 0.35, vec3(1.0), ink);
      }
      vec2 dots = abs(fract(p / 0.12) - 0.5);
      float px = mix(0.55 + 0.45 * smoothstep(0.45, 0.2, max(dots.x, dots.y)), 1.0, smoothstep(0.02, 0.08, fw.x));
      s.albedo = vec3(0.02); s.rough = 0.25; s.porosity = 0.0;
      s.emit = pow(c, vec3(2.2)) * px * (1.8 + 2.4 * night);
    } else if (st == 202) {  // rooftop billboard (generic graphic), flood-lit at night
      float ci = floor(lx / 1.6);
      float ink = glyphInk(vec2(fract(lx / 1.6), fract(m.y / 2.2)), ci, seed) * step(abs(lx), 5.0);
      vec3 face = mix(base, vec3(1.0), ink);
      s.albedo = pow(face, vec3(2.2)); s.rough = 0.5;
      s.emit = pow(face, vec3(2.2)) * 1.1 * night;
    } else if (st == 210) {
      s.albedo = vec3(0.02, 0.026, 0.032) + wall * 0.05; s.rough = 0.05; s.porosity = 0.0;
    } else if (st == 220) {
      s.albedo = wall; s.metal = 0.6; s.rough = 0.35; s.porosity = 0.0;
    } else if (st == 230) {
      s.albedo = wall; s.rough = 0.3; s.emit = wall * (0.4 + 6.0 * night);
    }
    return s;
  }
  // ---- walls ----
  float u = m.x - 1024.0, v = m.y;
  if (st == 0) {
    s.albedo = wall * (0.9 + 0.2 * nz.b);
    return s;
  }
  if (st == 1) {  // cladding panels
    vec2 j = abs(fract(vec2(u / 1.5, v / 1.2)) - 0.5);
    s.albedo = wall * (1.0 - 0.3 * smoothstep(0.46, 0.49, max(j.x, j.y)));
    s.rough = 0.55;
    return s;
  }
  if (st == 70) {  // temple: white plaster between vermilion posts, dark wood skirting
    float post = 1.0 - aaBand(0.07, 0.93, fract(u / 2.4 + 0.5), fw.x / 2.4);
    s.albedo = mix(wall, vec3(0.50, 0.05, 0.03), post);
    s.albedo = mix(s.albedo, vec3(0.12, 0.07, 0.04), step(v, 0.9));
    s.rough = 0.7;
    return s;
  }
  if (st == 32) {  // balcony railing panel (frosted glass + bars)
    float bar = 1.0 - aaBand(0.1, 0.9, fract(u / 0.12), fw.x / 0.12);
    s.albedo = mix(wall * 0.9 + 0.08, vec3(0.3), bar * 0.5);
    s.rough = 0.35; s.porosity = 0.0;
    return s;
  }
  if (st == 50) {  // warehouse: corrugated panels, big doors
    float c = sin(u * 6.2832 / 0.2);
    s.n = normalize(ng + T * c * 0.18 * (1.0 - smoothstep(0.02, 0.08, fw.x)));
    vec2 f = vec2(fract(u / 14.0 + 0.5), v);
    float door = rect2(f, vec2(0.32, 0.0), vec2(0.68, 5.5), vec2(fw.x / 14.0, fw.y));
    s.albedo = mix(wall * (0.92 + 0.1 * c), vec3(0.18, 0.2, 0.22), door);
    s.metal = 0.3; s.rough = 0.5; s.porosity = 0.1;
    return s;
  }
  // window grids: storey height, bay width, window rectangle inside the cell (fractions)
  float sh = 3.6, bw = 3.0, gf = 0.0;
  vec4 wr = vec4(0.2, 0.25, 0.8, 0.8);
  int occ = 0;
  float presence = 1.0;
  if (st == 10) { sh = 4.0; bw = 1.6; wr = vec4(0.02, 0.3, 0.98, 0.82); }
  else if (st == 11) { sh = 3.8; bw = 3.0; wr = vec4(0.18, 0.26, 0.82, 0.8); }
  else if (st == 12 || st == 13) { sh = 4.0; bw = 1.5; wr = vec4(0.03, 0.0, 0.97, 0.78); }
  else if (st == 20) { sh = 3.7; bw = 3.2; wr = vec4(0.12, 0.3, 0.88, 0.82); gf = 4.8; occ = 2; }
  else if (st == 21) { sh = 3.4; bw = 2.2; wr = vec4(0.2, 0.3, 0.8, 0.8); gf = 4.5; occ = 2; }
  else if (st == 30) { sh = 3.0; bw = 3.4; wr = vec4(0.1, 0.36, 0.9, 0.86); occ = 1; }
  else if (st == 31) { sh = 3.0; bw = 2.8; wr = vec4(0.3, 0.42, 0.72, 0.76); occ = 1; }
  else if (st == 40) { sh = 2.9; bw = 3.6; wr = vec4(0.26, 0.33, 0.72, 0.74); occ = 1; presence = 0.7; }
  else if (st == 41) { sh = 2.85; bw = 2.7; wr = vec4(0.25, 0.36, 0.75, 0.74); occ = 1; presence = 0.55; }
  else if (st == 60) { sh = 4.5; bw = 2.0; wr = vec4(0.04, 0.16, 0.96, 0.86); gf = 6.0; }
  else if (st == 80) { sh = 3.6; bw = 2.2; wr = vec4(0.08, 0.3, 0.92, 0.8); }
  bool ground = gf > 0.0 && v < gf;
  float vv = gf > 0.0 ? v - gf : v;
  vec2 cell = vec2(u / bw, vv / sh);
  vec2 cid = floor(cell), f = cell - cid;
  vec2 fwc = fw / vec2(bw, sh);
  float tiny = smoothstep(0.2, 0.55, max(fwc.x, fwc.y));
  float h = hash12(cid + seed * 97.0);
  float present = step(hash12(cid * 1.31 + seed * 5.0), presence);
  float win = rect2(f, wr.xy, wr.zw, fwc * 0.7) * present;
  float cover = (wr.z - wr.x) * (wr.w - wr.y) * presence;
  win = mix(win, cover, tiny);
  float frameM = (rect2(f, wr.xy - 0.03, wr.zw + 0.03, fwc * 0.7) * present - win) * (1.0 - tiny);
  float occv = occ == 0 ? occupancy.x : (occ == 1 ? occupancy.y : occupancy.z);
  float lit = step(h, occv);
  if (st == 12 || st == 13) lit = step(hash12(vec2(floor(u / 4.5), cid.y) + seed * 97.0), occupancy.x);
  vec3 room = fract(h * 7.3) < 0.55 ? vec3(1.0, 0.84, 0.62) : vec3(0.86, 0.93, 1.0);
  vec3 glass = vec3(0.022, 0.028, 0.034);
  if (st == 12 || st == 13) {  // curtain wall: tinted glass, spandrel band, mullions (and fins)
    glass = vec3(0.02, 0.025, 0.03) + wall * 0.07;
    float mull = (1.0 - aaBand(0.03, 0.97, f.x, fwc.x)) * (1.0 - tiny);
    s.albedo = mix(wall * 0.45, glass, win);
    s.albedo = mix(s.albedo, vec3(0.3, 0.31, 0.33), mull);
    s.rough = mix(0.3, 0.12, win); s.metal = 0.0; s.porosity = 0.0;
    if (st == 13) {
      float fin = sin(u * 6.2832 / 1.2);
      s.n = normalize(ng + T * fin * 0.2 * (1.0 - tiny));
    }
    s.emit = room * lit * win * night * (0.35 + 0.6 * fract(h * 3.1)) * (1.0 - 0.4 * tiny);
    return s;
  }
  if (st == 41) {  // wooden house: vertical boards, paper-screen windows with lattice
    float board = hash12(vec2(floor(u / 0.18), cid.y));
    vec3 wood = wall * (0.8 + 0.35 * board);
    vec2 lat = abs(fract(vec2(u, vv) / 0.3) - 0.5);
    float grid = smoothstep(0.44, 0.48, max(lat.x, lat.y)) * (1.0 - tiny);
    vec3 paper = mix(vec3(0.55, 0.52, 0.45), vec3(0.2, 0.14, 0.1), grid);
    paper = mix(paper, glass + vec3(0.04), step(0.5, fract(h * 3.3)));  // many houses have plain glass
    s.albedo = mix(wood, paper, win); s.rough = 0.75;
    s.emit = vec3(1.0, 0.78, 0.5) * lit * win * night * 0.5 * (1.0 - grid);
    return s;
  }
  if (st == 30) {  // apartment front: balcony railing band + sliding doors
    float fy = fract(v / sh);
    float rail = step(fy, 0.36) * (1.0 - tiny);
    s.albedo = mix(wall, glass, win);
    s.albedo = mix(s.albedo, wall * 0.92 + 0.05, rail);
    s.rough = mix(0.8, 0.14, win * (1.0 - rail));
    vec3 cur = mix(room, vec3(1.0, 0.9, 0.75), step(0.5, fract(h * 11.0)));
    s.emit = cur * lit * win * (1.0 - rail) * night * 0.55;
    return s;
  }
  if (ground) {  // shop level: glass front with a sign band on top, lit inside
    float gy = v / gf;
    float band = step(0.8, gy);
    float gl = rect2(vec2(fract(u / 2.0), gy), vec2(0.04, 0.03), vec2(0.96, 0.78), vec2(fw.x / 2.0, fw.y / gf) * 0.7);
    vec3 signc = hue(hash12(vec2(floor(u / 7.5), seed)));
    s.albedo = mix(mix(wall * 0.8, glass, gl), pow(signc, vec3(2.2)) * 0.7, band);
    s.rough = mix(0.7, 0.08, gl);
    float shopLit = step(hash12(vec2(floor(u / 7.5), seed * 3.0)), occupancy.z);
    s.emit = (vec3(1.0, 0.92, 0.8) * gl * 0.9 + pow(signc, vec3(2.2)) * band * 1.4) * shopLit * night;
    return s;
  }
  // punched / ribbon windows
  float siding = st == 40 ? 0.94 + 0.06 * step(0.5, fract(v / 0.2)) : 1.0;
  // weathering: slab line every storey, rain streaks below the windows, grime near the ground
  float slab = (1.0 - aaBand(0.015, 0.985, fract(vv / sh), fwc.y)) * (1.0 - tiny) * (st == 40 ? 0.0 : 1.0);
  float streak = step(wr.x, f.x) * step(f.x, wr.z) * step(f.y, wr.y) * (0.6 + 0.4 * texture(texNoise, vec2(m.x * 0.9, m.y * 0.05)).r);
  float grime = 1.0 - 0.28 * exp(-v / 1.8);
  vec3 wallc = wall * siding * (0.9 + 0.14 * nz.b) * (1.0 - 0.12 * streak * (1.0 - tiny)) * (1.0 - 0.22 * slab) * grime;
  // glass reflects the sky / street (rough enough for the environment probe), rooms faintly visible by day
  vec3 inside = mix(vec3(0.035, 0.032, 0.03), vec3(0.06, 0.055, 0.05), fract(h * 5.7));
  s.albedo = mix(wallc, glass + inside * (1.0 - night), win);
  s.albedo = mix(s.albedo, st == 40 ? vec3(0.8) : vec3(0.35, 0.36, 0.38), frameM);
  s.rough = mix(0.8, 0.14, win); s.porosity = mix(0.4, 0.0, win);
  s.emit = room * lit * win * night * (0.35 + 0.7 * fract(h * 3.1)) * (1.0 - 0.4 * tiny);
  return s;
}

// Ground raster -> material blend (asphalt / paving / paint / greenery), with world-space detail.
Surf terrainSurface(vec3 ng, vec2 wuv) {
  vec4 g = texture(texture0, fragUV);
  if (g.a < 0.5) discard;  // real openings (stairwells) in the pavement
  vec3 c = g.rgb;
  float lum = dot(c, vec3(0.3, 0.59, 0.11));
  float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
  float wMark = smoothstep(0.76, 0.84, lum);
  float wGreen = smoothstep(0.06, 0.12, c.g - max(c.r, c.b));
  float wRoad = (1.0 - smoothstep(0.30, 0.40, lum)) * (1.0 - wGreen);
  float wSand = smoothstep(0.12, 0.2, sat) * step(c.b, c.g) * step(c.g, c.r) * smoothstep(0.5, 0.58, lum) * (1.0 - wMark);
  float wPave = (1.0 - wRoad - wMark - wGreen - wSand);
  wPave = max(wPave, 0.0);
  vec4 macro = texture(texNoise, wuv / 48.0);
  // asphalt
  vec2 auv = wuv / 3.5;
  vec4 A = texture(texAsphalt, auv), N = texture(texAsphaltN, auv);
  float patch_ = smoothstep(0.66, 0.70, macro.a);
  vec3 asph = vec3(0.150, 0.150, 0.154) * A.rgb * (0.9 + 0.2 * macro.r) * mix(1.0, 0.8, patch_);
  float asphR = mix(A.a, 0.75, patch_);
  // paving (plazas, unraised sidewalks): lighter blocks, colour from the raster
  vec4 P = texture(texPaving, wuv / 1.2), PN = texture(texPavingN, wuv / 1.2);
  vec3 pave = pow(c, vec3(2.2)) * (P.rgb / 0.30) * (0.85 + 0.3 * macro.r);
  vec3 paint = vec3(0.74, 0.74, 0.70) * (0.85 + 0.15 * A.r);
  vec3 green = vec3(0.07, 0.10, 0.04) * (0.7 + 0.6 * A.g);
  Surf s;
  vec3 sand = pow(c, vec3(2.2)) * (0.85 + 0.3 * texture(texNoise, wuv / 0.7).r) * (0.9 + 0.2 * macro.g);
  s.albedo = asph * wRoad + pave * wPave + paint * wMark + green * wGreen + sand * wSand;
  s.rough = asphR * wRoad + P.a * wPave + 0.6 * wMark + 0.9 * wGreen + 0.95 * wSand;
  s.metal = 0.0;
  vec2 nxy = N.rg * wRoad + PN.rg * wPave + vec2(0.5) * (wMark + wGreen);
  s.n = tangentNormal(ng, nxy, 0.7);
  s.ao = mix(1.0, N.a, 0.6 * wRoad) * mix(1.0, PN.a, 0.8 * wPave) * texture(texAO, fragUV).r;
  s.emit = vec3(0.0);
  s.transmit = vec3(0.0);
  s.porosity = 0.8 * wRoad + 0.6 * wPave + 0.3 * wMark;
  // sea bed (below the water, seen at the waterline): wet sand and silt
  float isBed = 1.0 - smoothstep(0.02, 0.05, distance(c, vec3(52.0, 66.0, 62.0) / 255.0));
  if (isBed > 0.01) {
    s.albedo = mix(s.albedo, vec3(0.07, 0.065, 0.05) * (0.8 + 0.4 * macro.g), isBed);
    s.rough = mix(s.rough, 0.5, isBed); s.n = normalize(mix(s.n, ng, isBed));
  }
  if (landOn == 1) {
    // rural ground (fictional country): the land-cover weights say what grows here, the ground
    // raster's key colours keep levees, farm roads and verges as they are
    vec4 Lc = texture(texLand, fragUV);
    float isForest = (1.0 - smoothstep(0.035, 0.08, distance(c, vec3(46.0, 62.0, 36.0) / 255.0))) * smoothstep(0.2, 0.5, Lc.r);
    float isPaddy = (1.0 - smoothstep(0.03, 0.07, distance(c, vec3(92.0, 118.0, 64.0) / 255.0))) * smoothstep(0.2, 0.5, Lc.g);
    // the raster's two field colours (soil / rows) blend when filtered: accept the whole segment between them
    vec3 fk1 = vec3(124.0, 110.0, 76.0) / 255.0, fk2 = vec3(104.0, 92.0, 64.0) / 255.0, fe = fk2 - fk1;
    float fdist = distance(c, fk1 + fe * clamp(dot(c - fk1, fe) / dot(fe, fe), 0.0, 1.0));
    float isField = (1.0 - smoothstep(0.03, 0.07, fdist)) * smoothstep(0.2, 0.5, Lc.b);
    float isBare = (1.0 - smoothstep(0.04, 0.09, distance(c, vec3(86.0, 66.0, 58.0) / 255.0))) * smoothstep(0.2, 0.5, Lc.a);
    vec4 n1 = texture(texNoise, wuv / 2.3), n2 = texture(texNoise, wuv / 17.0);
    if (isForest > 0.01) {  // forest floor: litter and moss in the shade of the crowns
      vec3 a = vec3(0.034, 0.038, 0.020) * (0.7 + 0.6 * n1.r) * (0.85 + 0.3 * n2.g);
      s.albedo = mix(s.albedo, a, isForest); s.rough = mix(s.rough, 0.95, isForest); s.ao *= mix(1.0, 0.75, isForest);
    }
    if (isPaddy > 0.01) {
      // planting rows 30 cm apart along the plots (same orientation as the plot pattern)
      float ang = 0.35;
      vec2 ruv = vec2(wuv.x * cos(ang) + wuv.y * sin(ang), -wuv.x * sin(ang) + wuv.y * cos(ang));
      float aaRow = smoothstep(0.2, 0.55, fwidth(ruv.x) / 0.3);  // rows finer than a few pixels: their mean
      float row = mix(smoothstep(0.35, 0.05, abs(fract(ruv.x / 0.3) - 0.5) * 2.0), 0.2, aaRow);
      float tuft = 0.75 + 0.25 * texture(texNoise, ruv * vec2(3.3, 0.9)).r;
      // each plot (about 30 x 90 m) differs a little; late in the season some are already cut
      float plot = hash12(floor(vec2(ruv.x / 30.0, ruv.y / 90.0)) + 17.0);
      float cut = step(plot, clamp((cropStage - 2.75) * 3.0, 0.0, 1.0));
      vec3 water = vec3(0.020, 0.028, 0.026);
      vec3 mud = vec3(0.085, 0.070, 0.050) * (0.8 + 0.4 * n1.g);
      // cut stalks in their rows (broken along the row) over mud strewn with chopped straw
      float stalk = row * (0.5 + 0.5 * texture(texNoise, ruv * vec2(3.3, 7.0)).r);
      float chopped = smoothstep(0.4, 0.85, texture(texNoise, wuv / 0.9).g);
      vec3 stubble = mix(mud, vec3(0.16, 0.13, 0.08), clamp(0.2 + 0.4 * stalk + 0.35 * chopped, 0.0, 1.0));
      vec3 seedl = mix(water, vec3(0.05, 0.10, 0.03), row * 0.35);
      vec3 green = mix(water, vec3(0.055, 0.115, 0.028) * tuft, clamp(row * 1.4 + 0.45, 0.0, 1.0));
      vec3 gold = vec3(0.20, 0.155, 0.058) * (0.8 + 0.4 * n1.b) * tuft * (0.85 + 0.3 * plot);
      gold = mix(gold, vec3(0.13, 0.12, 0.05), smoothstep(0.35, 0.0, abs(fract(ruv.x / 0.3) - 0.5)) * 0.25 * (1.0 - aaRow));
      float st = cropStage;
      vec3 a = st < 1.0 ? mix(stubble, seedl, st) : st < 2.0 ? mix(seedl, green, st - 1.0) : mix(green, gold, st - 2.0);
      a = mix(a, stubble, cut);
      float r = st < 1.0 ? mix(0.9, 0.06, st) : st < 2.0 ? mix(0.06, 0.55, st - 1.0) : mix(0.55, 0.8, st - 2.0);
      s.albedo = mix(s.albedo, a, isPaddy); s.rough = mix(s.rough, r, isPaddy);
      s.porosity = mix(s.porosity, 0.0, isPaddy * step(0.5, st) * step(st, 1.6));
      s.n = normalize(mix(s.n, ng, isPaddy));
    }
    if (isField > 0.01) {
      // small upland plots (about 20 x 45 m) with their own crop and row direction: bare ploughed
      // soil, vegetable rows (green most of the year) or fallow grass
      float ang = -0.5;
      vec2 puv = vec2(wuv.x * cos(ang) + wuv.y * sin(ang), -wuv.x * sin(ang) + wuv.y * cos(ang));
      vec2 pid = floor(vec2(puv.x / 20.0, puv.y / 45.0));
      float h1 = hash12(pid + 31.0), h2 = hash12(pid + 57.0);
      bool across = h1 > 0.5;
      float u = across ? puv.y : puv.x;
      float period = 0.9 + 0.5 * h2;
      float f = fract(u / period);
      float aa = smoothstep(0.2, 0.55, fwidth(u) / period);  // rows finer than a few pixels fade to their mean
      float ridge = mix(smoothstep(0.42, 0.12, abs(f - 0.5)), 0.5, aa);
      float kind = hash12(pid + 83.0);  // < 0.35 bare, < 0.85 vegetables, else fallow
      float grow = cropStage < 1.0 ? 0.35 : smoothstep(1.0, 1.8, cropStage);
      vec3 soil = vec3(0.10, 0.075, 0.052) * (0.8 + 0.4 * n1.r) * (0.9 + 0.2 * h2);
      vec3 leaves = vec3(0.045, 0.085, 0.028) * (0.8 + 0.4 * n1.g) * (0.85 + 0.3 * h1);
      vec3 fallow = mix(vec3(0.07, 0.085, 0.035), vec3(0.11, 0.095, 0.05), step(2.6, cropStage) + step(cropStage, 0.9));
      vec3 a = kind < 0.35 ? soil : kind < 0.85 ? mix(soil, leaves, ridge * grow) : fallow * (0.85 + 0.3 * n1.b);
      s.albedo = mix(s.albedo, a, isField); s.rough = mix(s.rough, 0.92, isField);
      // furrow profile: the surface slopes across the rows (not for fallow plots)
      vec2 dirW = across ? vec2(-sin(ang), cos(ang)) : vec2(cos(ang), sin(ang));
      float slope = cos(6.2831 * f) * 0.22 * (1.0 - aa) * step(kind, 0.85);
      vec3 tilt = vec3(dirW.x, 0.0, -dirW.y) * slope;
      s.n = normalize(mix(s.n, normalize(ng + tilt), isField));
    }
    if (isBare > 0.01) {  // volcanic scoria and rock
      vec3 a = mix(vec3(0.09, 0.055, 0.045), vec3(0.13, 0.12, 0.11), smoothstep(0.4, 0.7, n2.r)) * (0.75 + 0.5 * n1.b);
      s.albedo = mix(s.albedo, a, isBare); s.rough = mix(s.rough, 0.95, isBare);
    }
  }
  // snow: open ground white; carriageways compacted and wet (tyre tracks)
  float sn = snowAt(fragPos, ng);
  if (sn > 0.001) {
    float onRoad = wRoad * (1.0 - wGreen);
    float amt = sn * mix(1.0, 0.45 + 0.35 * texture(texNoise, wuv / 3.1).r, onRoad);
    s.albedo = mix(s.albedo, snowAlbedo(wuv) * mix(1.0, 0.8, onRoad), amt);
    s.rough = mix(s.rough, mix(0.55, 0.3, onRoad), amt);
    s.porosity = mix(s.porosity, 0.2, amt);
  }
  return s;
}

// Far-view terrain (coarse tiles beyond the streamed cells): colour map with season and snow.
Surf farSurface(vec3 ng, vec2 wuv) {
  Surf s;
  vec3 c = texture(texture0, fragUV).rgb;
  s.albedo = pow(c, vec3(2.2)) * 0.95; s.rough = 0.9; s.metal = 0.0; s.n = ng; s.ao = 1.0; s.emit = vec3(0.0);
  s.porosity = 0.5; s.transmit = vec3(0.0);
  // towns (painted a flat grey in the colour map): a mottled roofscape of tiles, metal and concrete
  float town = 1.0 - smoothstep(0.03, 0.07, distance(c, vec3(122.0, 121.0, 110.0) / 255.0));
  if (town > 0.01) {
    vec4 t1 = texture(texNoise, wuv / 17.0), t2 = texture(texNoise, wuv / 73.0);
    vec3 roofs = mix(vec3(0.075, 0.078, 0.085), vec3(0.16, 0.15, 0.14), smoothstep(0.35, 0.75, t1.r)) * (0.8 + 0.4 * t2.g);
    s.albedo = mix(s.albedo, roofs, town);
  }
  float forest = 1.0 - smoothstep(0.04, 0.10, distance(c, vec3(40.0, 58.0, 32.0) / 255.0));
  if (forest > 0.01) {  // broadleaf patches turn in autumn and are bare in winter (as the near canopy)
    float broad = smoothstep(0.45, 0.65, texture(texNoise, wuv / 900.0).g);
    vec3 autumn = mix(vec3(0.26, 0.06, 0.02), vec3(0.30, 0.18, 0.03), texture(texNoise, wuv / 300.0).b);
    vec3 bare = vec3(0.07, 0.055, 0.045);
    vec3 leaf = leafStage < 1.0 ? mix(s.albedo, autumn, leafStage) : mix(autumn, bare, leafStage - 1.0);
    s.albedo = mix(s.albedo, leaf, broad * forest);
  }
  float paddy = 1.0 - smoothstep(0.03, 0.08, distance(c, vec3(92.0, 118.0, 64.0) / 255.0));
  if (paddy > 0.01) {
    vec3 a = cropStage < 1.0 ? vec3(0.10, 0.08, 0.055) : cropStage < 2.0 ? mix(vec3(0.03, 0.045, 0.04), s.albedo, cropStage - 1.0)
                              : mix(s.albedo, vec3(0.28, 0.22, 0.07), cropStage - 2.0);
    s.albedo = mix(s.albedo, a, paddy);
  }
  float sn = snowAt(fragPos, ng);
  s.albedo = mix(s.albedo, vec3(0.80, 0.83, 0.88), sn); s.rough = mix(s.rough, 0.6, sn);
  return s;
}

void main() {
  vec3 ng = normalize(fragNormal);
  if (!gl_FrontFacing) ng = -ng;
  vec2 wuv = vec2(fragPos.x, -fragPos.z);  // world east / north (m)
  int id = materialOverride >= 0 ? materialOverride : int(fragMat.x + 0.5);
  if (id == 17 && canopyCut > 0.0 && fragColor.b > 0.05 && fragColor.b < 0.95) {  // the canopy near the camera: single trees there
    float d = length(fragPos.xz - viewPos.xz);
    if (d < canopyCut) discard;
  }
  Surf s;
  if (surfaceMode == 1) {
    s = terrainSurface(ng, wuv);
  } else if (surfaceMode == 3) {
    s = facade(ng);
  } else if (surfaceMode == 4) {
    s = farSurface(ng, wuv);
  } else {
    s = material(id, ng, wuv);
    if (useTexture == 1) {
      vec4 t = texture(texture0, fragUV);
      if (t.a < 0.5) discard;
      s.albedo *= pow(t.rgb, vec3(2.2));
      if (surfaceMode == 2) {
        // Glass in the facade photos (cool, fairly dark, unsaturated texels on vertical faces) is glossy
        // so curtain walls pick up sky and street reflections; the rest stays matte.
        float mxg = max(t.r, max(t.g, t.b)), mng = min(t.r, min(t.g, t.b));
        float cool = smoothstep(0.0, 0.06, t.b - t.r);
        float glassM = cool * (1.0 - smoothstep(0.35, 0.6, mxg)) * (1.0 - smoothstep(0.25, 0.45, mxg - mng)) * step(abs(ng.y), 0.3);
        s.rough = mix(0.7, 0.12, glassM); s.porosity = mix(0.4, 0.0, glassM);
        // Signage in the real facade photos (bright, saturated texels) is lit at night.
        float mx = max(t.r, max(t.g, t.b)), mn = min(t.r, min(t.g, t.b));
        float sgn = smoothstep(0.30, 0.55, mx - mn) * smoothstep(0.40, 0.75, mx);
        s.emit += pow(t.rgb, vec3(2.2)) * sgn * nightFactor * 4.0;
        // Lit windows at night (procedural, not in the photo): storey (3.5 m) x bay (2.6 m) cells on
        // vertical faces, lit with the time-of-day office occupancy, only where the photo texel is
        // glass-like (darker / cooler than the wall).
        if (nightFactor > 0.01 && abs(ng.y) < 0.3) {
          vec3 hn = normalize(vec3(ng.x, 0.0, ng.z));
          float along = dot(fragPos, vec3(-hn.z, 0.0, hn.x));
          vec2 g = vec2(along / 1.8, fragPos.y / 3.5);
          vec2 cid = floor(g), f = fract(g);
          float h = hash12(cid + floor(hn.xz * 8.0) * 31.0);
          float lit = step(h, occupancy.x * 0.8);
          float inWin = step(0.08, f.x) * step(f.x, 0.92) * step(0.2, f.y) * step(f.y, 0.86);
          float lum = dot(t.rgb, vec3(0.3, 0.59, 0.11));
          float glassy = mix(0.08, 1.0, 1.0 - smoothstep(0.18, 0.42, lum)) * (0.6 + 0.4 * step(t.r, t.b + 0.02));
          vec3 wc = fract(h * 7.31) < 0.6 ? vec3(0.9, 0.95, 1.0) : vec3(1.0, 0.82, 0.6);  // LED office / warm
          s.emit += wc * lit * inWin * glassy * nightFactor * (0.35 + 0.75 * fract(h * 3.7));
        }
      }
    }
  }
  vec3 V = normalize(viewPos - fragPos);
  // Rain: darker, glossier porous surfaces; standing water on flat ground.
  if (wetness > 0.0) {
    float up = smoothstep(0.6, 0.95, s.n.y);
    float wet = wetness * mix(0.55, 1.0, up);
    s.albedo *= mix(1.0, 0.45 + 0.4 * (1.0 - s.porosity), wet * s.porosity + wet * 0.2);
    s.rough = mix(s.rough, 0.06, wet * 0.85);
    float puddle = up * smoothstep(0.62, 0.72, texture(texNoise, wuv / 9.0).g + (wetness - 0.6) * 0.5) * wetness;
    if (surfaceMode == 1 || id == 2 || id == 15) {
      s.n = normalize(mix(s.n, ng, puddle));
      s.rough = mix(s.rough, 0.015, puddle);
      s.albedo *= mix(1.0, 0.6, puddle);
      // rain ripples
      vec2 rc = floor(wuv * 3.0);
      float rt = fract(timeSec * 1.3 + hash12(rc));
      float rr = length(fract(wuv * 3.0) - 0.5) - rt * 0.5;
      s.n = normalize(s.n + vec3(0.0, 0.0, 0.0) + vec3(rr, 0.0, rr) * puddle * (1.0 - rt) * 0.15 * step(abs(rr), 0.05));
    }
  }
  vec3 n = s.n;
  float NdV = max(dot(n, V), 1e-3);
  vec3 F0 = mix(vec3(0.04), s.albedo, s.metal);
  float a = max(s.rough * s.rough, 0.002);
  float a2 = a * a;
  vec3 kdiff = (1.0 - s.metal) * s.albedo;
  // Sun (GGX) with cloud shadows.
  vec3 col = vec3(0.0);
  float NdL = dot(n, sunDir);
  if (NdL > 0.0 && dot(sunColor, vec3(1.0)) > 0.001) {
    vec3 H = normalize(V + sunDir);
    float NdH = max(dot(n, H), 0.0), VdH = max(dot(V, H), 0.0);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);
    float D = a2 / (PI * pow(NdH * NdH * (a2 - 1.0) + 1.0, 2.0));
    float k = (s.rough + 1.0) * (s.rough + 1.0) / 8.0;
    float G = (NdL / (NdL * (1.0 - k) + k)) * (NdV / (NdV * (1.0 - k) + k));
    vec3 spec = D * G * F / (4.0 * NdL * NdV + 1e-4);
    float sh = sunShadow(ng) * mix(1.0, 0.0, indoor);
    if (sunDir.y > 0.02) {
      vec2 cp = fragPos.xz + sunDir.xz / sunDir.y * (1800.0 - fragPos.y);
      sh *= 1.0 - 0.72 * cloudDensityAt(vec2(cp.x, -cp.y));
    }
    col += (kdiff * (1.0 - F) + PI * spec) * sunColor * NdL * sh;
  }
  if (id == 33 && surfaceMode == 0) {
    float back = max(dot(-n, sunDir), 0.0);
    col += s.albedo * vec3(1.0, 1.05, 0.7) * sunColor * back * 0.45 * sunShadow(ng);
  }
  // Ambient: hemisphere diffuse + sky reflection (split-sum-ish approximation).
  float hemi = n.y * 0.5 + 0.5;
  vec3 irr = mix(ambientGround, ambientSky, hemi);
  col += kdiff * irr * s.ao;
  vec3 R = reflect(-V, n);
  vec3 Fr = F0 + (max(vec3(1.0 - s.rough), F0) - F0) * pow(1.0 - NdV, 5.0);
  // Glossy reflections. Mirror-like surfaces (glass, puddles, wet asphalt) are resolved in screen
  // space from the real scene (SSR post pass, amount written to alpha); rough surfaces use a smooth
  // street-canyon probe (buildings low in the reflected view, sky above).
  float bldg = 1.0 - smoothstep(0.05, 0.28, R.y);
  vec3 envSharp = mix(skyRadiance(R), ambientGround * 0.75 + ambientSky * 0.15, bldg);
  vec3 env = mix(envSharp, irr * 1.1, clamp(s.rough * 1.2, 0.0, 1.0));
  env = mix(env, irr * 0.9, indoor);
  float specOcc = clamp(pow(NdV + s.ao, 2.0) - 1.0 + s.ao, 0.0, 1.0);
  float mirror = (1.0 - smoothstep(0.07, 0.25, s.rough)) * (1.0 - indoor);
  col += env * Fr * specOcc * (1.0 - 0.6 * s.rough) * (1.0 - mirror);
  float reflAmt = clamp(dot(Fr, vec3(0.3333)) * specOcc, 0.0, 1.0) * mirror;
  // Artificial lights (street lights, shops) near the camera.
  for (int i = 0; i < numLights; i++) {
    vec3 Lv = lightPosR[i].xyz - fragPos;
    float d = length(Lv);
    float r = lightPosR[i].w;
    if (d > r) continue;
    vec3 L = Lv / d;
    float nl = max(dot(n, L), 0.0);
    if (nl <= 0.0) continue;
    float att = pow(clamp(1.0 - pow(d / r, 4.0), 0.0, 1.0), 2.0) / (d * d + 1.0);
    vec3 H = normalize(V + L);
    float NdH = max(dot(n, H), 0.0);
    float D = a2 / (PI * pow(NdH * NdH * (a2 - 1.0) + 1.0, 2.0));
    col += (kdiff + F0 * D * 0.25) * lightCol[i] * nl * att;
  }
  // Interior lighting (lit vehicle interiors): ceiling light strips, strongest on upward faces.
  col += kdiff * selfLight * s.ao * (0.6 + 0.4 * n.y);
  col += s.emit;
  // Light from inside, through glass (less at grazing angles where the reflection dominates).
  col += s.transmit * (1.0 - (0.04 + 0.96 * pow(1.0 - NdV, 5.0)));
  // Aerial perspective: distant surfaces take the colour of the sky behind them.
  float dist = length(viewPos - fragPos);
  float hf = exp(-max(fragPos.y - viewPos.y, 0.0) * 0.004);
  float fog = 1.0 - exp(-dist * fogDensity * mix(0.6, 1.0, hf));
  vec3 fc = mix(skyRadiance(normalize(vec3(-V.x, max(-V.y, 0.02), -V.z))), hazeColor, 0.35);
  col = mix(col, fc, clamp(fog, 0.0, 1.0));
  finalColor = vec4(toneMap(col), reflAmt * (1.0 - clamp(fog, 0.0, 1.0)));
}
)";

inline const char* kDepthVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 fragUV;
void main() { fragUV = vertexTexCoord; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
inline const char* kDepthFs = R"(#version 330
in vec2 fragUV;
uniform sampler2D texture0;
out vec4 finalColor;
void main() {
  if (texture(texture0, fragUV).a < 0.5) discard;  // alpha-tested foliage
  finalColor = vec4(1.0);
}
)";

// ---------------------------------------------------------------------------
// Sky: atmosphere approximation, sun disc, cumulus layer, cirrus streaks.
inline const char* kSkyVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 uv;
void main() { uv = vertexTexCoord; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
inline const char* kSkyFs = "#version 330\n" RJ_COMMON_GLSL R"(
in vec2 uv;
uniform vec2 resolution;
uniform mat4 invViewProj;
uniform vec3 camPos;
uniform float sunVisible;
uniform float starBright;
uniform vec3 cityGlow;  // light of the city on the cloud bases at night
out vec4 finalColor;
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
void main() {
  vec2 ndc = gl_FragCoord.xy / resolution * 2.0 - 1.0;
  vec4 w = invViewProj * vec4(ndc, 1.0, 1.0);
  vec3 d = normalize(w.xyz / w.w - camPos);
  vec3 col = skyRadiance(d);
  float mu = dot(d, sunDir);
  // Stars (night, clear sky only).
  if (d.y > 0.0 && starBright > 0.0) {
    vec2 sp = floor(d.xz / (d.y + 0.35) * 420.0);
    float st = step(0.9993, hash12(sp));  // Tokyo: only the brightest stars show through the sky glow
    col += vec3(st) * starBright * (1.0 - cloudCover) * smoothstep(0.0, 0.3, d.y);
  }
  // Sun disc with limb darkening (0.27 deg radius), dimmed by clouds.
  float disc = smoothstep(0.99996, 0.99999, mu);
  float cloudAtSun = 0.0;
  if (d.y > 0.0) {
    // Cumulus layer at 1.8 km: density along the view ray.
    float t = (1800.0 - camPos.y) / max(d.y, 0.01);
    vec2 p = camPos.xz + d.xz * t;
    float dens = cloudDensityLod(vec2(p.x, -p.y), 1.0 - smoothstep(0.05, 0.4, d.y));
    float fade = smoothstep(0.0, 0.12, d.y);
    // light: sunlit tops vs shadowed bases, silver lining near the sun
    vec3 lit = sunColor * (0.55 + 0.45 * pow(max(mu, 0.0), 4.0)) + ambientSky * 1.6;
    vec3 shade = ambientSky * 0.9 + skyHorizon * 0.25;
    float thick = texture(texNoise, (p + cloudOffset) * 0.00031 + 0.2).g;
    vec3 cc = mix(lit, shade, clamp(thick * 1.2 * (0.4 + cloudCover), 0.0, 1.0)) + cityGlow * (0.8 + 0.6 * thick);
    cc = mix(cc, skyRadiance(normalize(vec3(d.x, 0.02, d.z))), 1.0 - fade);
    col = mix(col, cc, dens * fade);
    cloudAtSun = dens * fade;
    // Cirrus: thin high streaks at 8 km.
    float t2 = (8000.0 - camPos.y) / max(d.y, 0.01);
    vec2 p2 = (camPos.xz + d.xz * t2 + cloudOffset * 2.0) * vec2(0.00006, 0.00018);
    float ci = smoothstep(0.55, 0.85, texture(texNoise, p2).b) * (1.0 - cloudCover) * smoothstep(0.02, 0.25, d.y);
    col = mix(col, sunColor * 0.35 + skyHorizon * 0.6, ci * 0.35);
  }
  col += vec3(1.0, 0.95, 0.85) * sunColor * 18.0 * disc * sunVisible * (1.0 - cloudAtSun);
  finalColor = vec4(toneMap(col), 0.0);  // alpha = reflection amount (none for the sky)
}
)";

// ---------------------------------------------------------------------------
// Post processing (fullscreen passes, raylib default vertex shader).
inline const char* kSsaoFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;  // scene depth
uniform vec2 invRes;         // 1 / depth texture size
uniform float nearZ;
uniform float farZ;
uniform vec2 tanHalf;        // tan(fov/2) * (aspect, 1)
out vec4 finalColor;
float linDepth(vec2 uv) {
  float z = texture(texture0, uv).r * 2.0 - 1.0;
  return 2.0 * nearZ * farZ / (farZ + nearZ - z * (farZ - nearZ));
}
vec3 viewPos(vec2 uv) {
  float d = linDepth(uv);
  return vec3((uv * 2.0 - 1.0) * tanHalf * d, -d);
}
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
void main() {
  // This pass runs at half resolution: its pixel centres fall on the corners of the full-resolution
  // depth texels, where nearest sampling rounds either way from row to row (bands on flat ground).
  vec2 uv = (floor(fragTexCoord / invRes) + 0.5) * invRes;
  float d0 = linDepth(uv);
  if (d0 > farZ * 0.9) { finalColor = vec4(1.0); return; }
  vec3 P = viewPos(uv);
  // normal from the neighbour on the flatter side in each axis (no false creases at depth steps and silhouettes)
  vec3 Pr = viewPos(uv + vec2(invRes.x, 0.0)) - P, Pl = P - viewPos(uv - vec2(invRes.x, 0.0));
  vec3 Pu = viewPos(uv + vec2(0.0, invRes.y)) - P, Pd = P - viewPos(uv - vec2(0.0, invRes.y));
  vec3 Px = abs(Pr.z) < abs(Pl.z) ? Pr : Pl;
  vec3 Py = abs(Pu.z) < abs(Pd.z) ? Pu : Pd;
  vec3 N = normalize(cross(Px, Py));
  float radius = clamp(0.6 + d0 * 0.02, 0.6, 3.0);  // metres, grows with distance
  float ang = hash12(gl_FragCoord.xy) * 6.2831;
  float occ = 0.0;
  const int S = 12;
  for (int i = 0; i < S; i++) {
    float fi = float(i) + 0.5;
    float a = ang + fi * 2.39996;
    float r = radius * sqrt(fi / float(S));
    vec3 dir = vec3(cos(a), sin(a), 0.0);
    // sample in the tangent disc, pushed along the normal
    vec3 T = normalize(dir - N * dot(dir, N));
    vec3 Sp = P + (T * r + N * r * 0.35);
    vec2 suv = (Sp.xy / (-Sp.z) / tanHalf) * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
    float sd = linDepth(suv);
    float diff = (-Sp.z) - sd;
    occ += (diff > 0.04 + 0.006 * d0 ? 1.0 : 0.0) * smoothstep(0.0, 1.0, radius / max(abs(d0 - sd), 1e-3));
  }
  float ao = 1.0 - occ / float(S);
  ao = mix(ao, 1.0, smoothstep(45.0, 90.0, d0));  // contact / crevice occlusion is a near-field effect
  finalColor = vec4(vec3(ao), 1.0);
}
)";

// Screen-space reflections: march the reflected ray against the depth buffer; misses fall back
// to the sky / city colours (already tone-mapped, like the scene target).
inline const char* kSsrFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;  // scene colour, alpha = reflection amount
uniform sampler2D texDepth;
uniform vec2 invRes;
uniform float nearZ;
uniform float farZ;
uniform vec2 tanHalf;
uniform mat4 invView;
uniform vec3 fbZenith;
uniform vec3 fbHorizon;
uniform vec3 fbCity;
out vec4 finalColor;
float linDepth(vec2 uv) {
  float z = texture(texDepth, uv).r * 2.0 - 1.0;
  return 2.0 * nearZ * farZ / (farZ + nearZ - z * (farZ - nearZ));
}
vec3 viewPos(vec2 uv) { float d = linDepth(uv); return vec3((uv * 2.0 - 1.0) * tanHalf * d, -d); }
vec2 project(vec3 p) { return (p.xy / (-p.z) / tanHalf) * 0.5 + 0.5; }
void main() {
  vec2 uv = (floor(fragTexCoord / invRes) + 0.5) * invRes;  // full-resolution texel centre (see the SSAO pass)
  float amt = texture(texture0, uv).a;
  if (amt < 0.01) { finalColor = vec4(0.0); return; }
  vec3 P = viewPos(uv);
  vec3 N = normalize(cross(viewPos(uv + vec2(invRes.x, 0.0)) - P, viewPos(uv + vec2(0.0, invRes.y)) - P));
  if (dot(N, -P) < 0.0) N = -N;
  vec3 Rv = normalize(reflect(normalize(P), N));
  vec3 Rw = mat3(invView) * Rv;
  vec3 sky = mix(fbHorizon, fbZenith, pow(clamp(Rw.y, 0.0, 1.0), 0.5));
  vec3 fb = mix(fbCity, sky, smoothstep(0.04, 0.26, Rw.y));
  vec3 p = P + N * 0.03;
  float stepLen = 0.2 + 0.01 * (-P.z);
  float travelled = 0.0;
  vec3 hitCol = fb;
  for (int i = 0; i < 40; i++) {
    vec3 prev = p;
    p += Rv * stepLen;
    travelled += stepLen;
    stepLen *= 1.1;
    if (-p.z < nearZ) break;
    vec2 suv = project(p);
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
    float dz = (-p.z) - linDepth(suv);
    if (dz > 0.0 && dz < max(0.35, stepLen * 2.0)) {
      vec3 a = prev, b = p;
      for (int k = 0; k < 5; k++) {
        vec3 m = (a + b) * 0.5;
        if ((-m.z) - linDepth(project(m)) > 0.0) b = m; else a = m;
      }
      vec2 h = project(b);
      float edge = smoothstep(0.0, 0.06, min(min(h.x, 1.0 - h.x), min(h.y, 1.0 - h.y)));
      float conf = edge * (1.0 - smoothstep(60.0, 120.0, travelled)) * smoothstep(-0.95, -0.2, -Rv.z);
      hitCol = mix(fb, texture(texture0, h).rgb, conf);
      break;
    }
  }
  finalColor = vec4(hitCol, amt);
}
)";

inline const char* kBlurFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 dir;  // texel step
out vec4 finalColor;
void main() {
  vec4 c = texture(texture0, fragTexCoord) * 0.227027;
  c += texture(texture0, fragTexCoord + dir * 1.3846) * 0.316216;
  c += texture(texture0, fragTexCoord - dir * 1.3846) * 0.316216;
  c += texture(texture0, fragTexCoord + dir * 3.2308) * 0.070270;
  c += texture(texture0, fragTexCoord - dir * 3.2308) * 0.070270;
  finalColor = c;
}
)";

inline const char* kBrightFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float threshold;
out vec4 finalColor;
void main() {
  vec3 c = texture(texture0, fragTexCoord).rgb;
  float l = max(c.r, max(c.g, c.b));
  finalColor = vec4(c * smoothstep(threshold, threshold + 0.15, l), 1.0);
}
)";

inline const char* kCompositeFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;  // scene colour (tone-mapped)
uniform sampler2D texAO;
uniform sampler2D texBloom;
uniform sampler2D texSsr;
uniform float aoStrength;
uniform float bloomStrength;
uniform vec3 whiteBalance;   // per-channel gain (colour temperature)
uniform float saturation;
uniform float contrast;
uniform float vignette;
uniform float rainOverlay;
uniform int debugView;       // 1 = reflection amount (scene alpha), 2 = SSAO, 3 = SSR
uniform float timeSec;
uniform vec2 invRes;
out vec4 finalColor;
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
// FXAA (Lottes, simplified console variant): the scene target is not multisampled.
vec3 fxaa(vec2 uv) {
  const vec3 LUMA = vec3(0.299, 0.587, 0.114);
  vec3 rgbNW = texture(texture0, uv + vec2(-1.0, -1.0) * invRes).rgb;
  vec3 rgbNE = texture(texture0, uv + vec2(1.0, -1.0) * invRes).rgb;
  vec3 rgbSW = texture(texture0, uv + vec2(-1.0, 1.0) * invRes).rgb;
  vec3 rgbSE = texture(texture0, uv + vec2(1.0, 1.0) * invRes).rgb;
  vec3 rgbM = texture(texture0, uv).rgb;
  float lNW = dot(rgbNW, LUMA), lNE = dot(rgbNE, LUMA), lSW = dot(rgbSW, LUMA), lSE = dot(rgbSE, LUMA), lM = dot(rgbM, LUMA);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * invRes;
  vec3 A = 0.5 * (texture(texture0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(texture0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 B = A * 0.5 + 0.25 * (texture(texture0, uv - dir * 0.5).rgb + texture(texture0, uv + dir * 0.5).rgb);
  float lB = dot(B, LUMA);
  return (lB < lMin || lB > lMax) ? A : B;
}
void main() {
  vec2 uv = fragTexCoord;
  vec3 c = fxaa(uv);
  // AO / bloom / SSR targets went through an odd number of render-target blits (each flips Y).
  vec2 uvf = vec2(uv.x, 1.0 - uv.y);
  float ao = texture(texAO, uvf).r;
  float lum0 = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c *= mix(1.0, ao, aoStrength * (1.0 - 0.5 * smoothstep(0.6, 0.95, lum0)));
  vec4 ssr = texture(texSsr, uvf);
  c += ssr.rgb * ssr.a;
  c += texture(texBloom, uvf).rgb * bloomStrength;
  c *= whiteBalance;
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(l), c, saturation);
  c = clamp((c - 0.5) * contrast + 0.5, 0.0, 1.0);
  vec2 q = uv - 0.5;
  c *= 1.0 - vignette * dot(q, q) * 1.6;
  // falling rain streaks (screen space, subtle)
  if (rainOverlay > 0.0) {
    vec2 ruv = vec2(uv.x * 90.0, uv.y * 7.0 + timeSec * 9.0);
    float cell = hash12(floor(ruv));
    float streak = step(0.93, cell) * smoothstep(0.0, 0.4, fract(ruv.y)) * (1.0 - fract(ruv.y));
    c = mix(c, vec3(0.75, 0.78, 0.82), streak * 0.18 * rainOverlay);
  }
  c += (hash12(gl_FragCoord.xy + timeSec) - 0.5) / 255.0;  // dither against banding
  if (debugView == 1) c = vec3(texture(texture0, uv).a);
  if (debugView == 2) c = vec3(ao);
  if (debugView == 3) c = ssr.rgb * ssr.a * 4.0;
  finalColor = vec4(c, 1.0);
}
)";

}  // namespace rjc::shaders
