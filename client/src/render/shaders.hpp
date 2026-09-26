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
uniform int numLights;
uniform vec4 lightPosR[24];   // xyz, range
uniform vec3 lightCol[24];
uniform vec3 emissiveTint;    // per-draw emission (signal lamps)
uniform vec3 occupancy;       // fraction of lit windows: office, residential, shop (by time of day)
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
      float b = mix(0.10, 0.9, nightFactor);
      vec3 tint = fragMat.y < 1.0 ? vec3(0.88, 0.94, 1.0) : vec3(1.0, 0.84, 0.62);  // office LED vs home
      s.transmit = interiorBehind(ng, false, lit) * tint * b;
    }
    if (id == 21) {
      float b = mix(0.28, 1.25, nightFactor);
      s.transmit = interiorBehind(ng, true, lit) * b;
    }
  } else if (id == 12) { s.albedo *= 0.85 + 0.3 * nz.b; s.rough = 0.9; s.porosity = 0.6; }
  else if (id == 13) { s.rough = 0.38; s.metal = 0.15; s.porosity = 0.0; }
  else if (id == 14) { s.albedo = vec3(0.30, 0.20, 0.11); s.metal = 1.0; s.rough = 0.38; s.porosity = 0.0; }
  else if (id == 22) { s.rough = 0.32; s.metal = 0.85; s.porosity = 0.0; }             // aluminium frames
  else if (id == 23) { s.rough = 0.9; s.porosity = 0.3; }                               // awning fabric
  else if (id == 24) { s.rough = 0.4; s.emit = vc * 2.2 * nightFactor * litFrom(fragMat.y); }  // back-lit sign band
  else if (id == 25) { s.rough = 0.55; s.metal = 0.2; s.porosity = 0.0; }             // AC outdoor unit
  else if (id == 26) { s.rough = 0.6; s.metal = 0.4; s.porosity = 0.0; }              // balcony rail
  else if (id == 27) { s.albedo *= 0.9 + 0.2 * nz.b; s.rough = 0.85; }                // wall paint
  else if (id == 28) { s.rough = 0.22; s.metal = 0.45; s.porosity = 0.0; }            // car paint
  else if (id == 29) { s.albedo = vec3(0.02); s.rough = 0.9; }                        // tyre
  else if (id == 30) { s.albedo = vec3(0.02); s.rough = 0.25; s.emit = emissiveTint; } // signal lamp lens
  else if (id == 31) { s.rough = 0.92; s.porosity = 0.8; }                            // clothing
  else if (id == 32) { s.rough = 0.55; }
  else if (id == 33) { s.rough = 0.7; s.porosity = 0.1; }                           // leaves
  else if (id == 34) { s.albedo *= 0.8 + 0.3 * nz.b; s.rough = 0.92; s.porosity = 0.5; } // bark
  else if (id == 35) { s.albedo = pow(fragColor.rgb, vec3(2.2)); s.rough = 0.45; s.porosity = 0.0; }  // untinted
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
  float wPave = (1.0 - wRoad - wMark - wGreen);
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
  s.albedo = asph * wRoad + pave * wPave + paint * wMark + green * wGreen;
  s.rough = asphR * wRoad + P.a * wPave + 0.6 * wMark + 0.9 * wGreen;
  s.metal = 0.0;
  vec2 nxy = N.rg * wRoad + PN.rg * wPave + vec2(0.5) * (wMark + wGreen);
  s.n = tangentNormal(ng, nxy, 0.7);
  s.ao = mix(1.0, N.a, 0.6 * wRoad) * mix(1.0, PN.a, 0.8 * wPave) * texture(texAO, fragUV).r;
  s.emit = vec3(0.0);
  s.transmit = vec3(0.0);
  s.porosity = 0.8 * wRoad + 0.6 * wPave + 0.3 * wMark;
  return s;
}

void main() {
  vec3 ng = normalize(fragNormal);
  if (!gl_FrontFacing) ng = -ng;
  vec2 wuv = vec2(fragPos.x, -fragPos.z);  // world east / north (m)
  int id = materialOverride >= 0 ? materialOverride : int(fragMat.x + 0.5);
  Surf s;
  if (surfaceMode == 1) {
    s = terrainSurface(ng, wuv);
  } else {
    s = material(id, ng, wuv);
    if (useTexture == 1) {
      vec4 t = texture(texture0, fragUV);
      if (t.a < 0.5) discard;
      s.albedo *= pow(t.rgb, vec3(2.2));
      if (surfaceMode == 2) {
        s.rough = 0.7; s.porosity = 0.4;
        // Signage in the real facade photos (bright, saturated texels) is lit at night.
        float mx = max(t.r, max(t.g, t.b)), mn = min(t.r, min(t.g, t.b));
        float sgn = smoothstep(0.30, 0.55, mx - mn) * smoothstep(0.40, 0.75, mx);
        s.emit += pow(t.rgb, vec3(2.2)) * sgn * nightFactor * 4.0;
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
    float st = step(0.9975, hash12(sp));
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
    vec3 cc = mix(lit, shade, clamp(thick * 1.2 * (0.4 + cloudCover), 0.0, 1.0));
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
  vec2 uv = fragTexCoord;
  float d0 = linDepth(uv);
  if (d0 > farZ * 0.9) { finalColor = vec4(1.0); return; }
  vec3 P = viewPos(uv);
  vec3 Px = viewPos(uv + vec2(invRes.x, 0.0)) - P;
  vec3 Py = viewPos(uv + vec2(0.0, invRes.y)) - P;
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
  vec2 uv = fragTexCoord;
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
uniform int debugView;       // 1 = reflection amount (scene alpha)
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
  finalColor = vec4(c, 1.0);
}
)";

}  // namespace rjc::shaders
