#include "render/textures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rjc {
namespace {

uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float rnd(int x, int y, uint32_t seed) { return static_cast<float>(hash2(x, y, seed) & 0xffffff) / 16777215.0f; }

// Tileable value noise with period `per` lattice cells.
float vnoise(float x, float y, int per, uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
  const float fx = x - xi, fy = y - yi;
  auto w = [](float t) { return t * t * (3.0f - 2.0f * t); };
  auto at = [&](int a, int b) { return rnd(((a % per) + per) % per, ((b % per) + per) % per, seed); };
  const float a = at(xi, yi), b = at(xi + 1, yi), c = at(xi, yi + 1), d = at(xi + 1, yi + 1);
  const float u = w(fx), v = w(fy);
  return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}
float fbm(float u, float v, int base_per, int octaves, uint32_t seed) {
  float s = 0, amp = 0.5f, norm = 0;
  int per = base_per;
  for (int o = 0; o < octaves; ++o) {
    s += amp * vnoise(u * per, v * per, per, seed + o * 101u);
    norm += amp;
    amp *= 0.5f;
    per *= 2;
  }
  return s / norm;
}
unsigned char u8(float v) { return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

Texture2D upload(std::vector<unsigned char>& px, int n) {
  Image img{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  Texture2D t = LoadTextureFromImage(img);
  GenTextureMipmaps(&t);
  SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_8X);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  return t;
}

// Normal map from a tileable height field (central differences, wrap).
void heightToNormal(const std::vector<float>& h, int n, float strength, std::vector<unsigned char>& px) {
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      auto H = [&](int a, int b) { return h[static_cast<size_t>(((b + n) % n) * n + ((a + n) % n))]; };
      const float dx = (H(x + 1, y) - H(x - 1, y)) * strength;
      const float dy = (H(x, y + 1) - H(x, y - 1)) * strength;
      // image y grows downwards; texture v grows up (north)
      float nx = -dx, ny = dy, nz = 1.0f;
      const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= l;
      ny /= l;
      const size_t i = static_cast<size_t>(y * n + x) * 4;
      px[i] = u8(nx * 0.5f + 0.5f);
      px[i + 1] = u8(ny * 0.5f + 0.5f);
    }
}

}  // namespace

bool DetailTextures::generate(int n) {
  std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4);
  // --- noise ---------------------------------------------------------------
  {
    // Repair patches: a few random axis-aligned rectangles with soft edges (tileable).
    struct Rect { float x, y, w, h; };
    std::vector<Rect> rects;
    for (int k = 0; k < 7; ++k)
      rects.push_back({rnd(k, 1, 91), rnd(k, 2, 91), 0.04f + 0.12f * rnd(k, 3, 91), 0.03f + 0.08f * rnd(k, 4, 91)});
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float u = static_cast<float>(x) / n, v = static_cast<float>(y) / n;
        float patch = 0.0f;
        for (const auto& r : rects) {
          float dx = std::fabs(u - r.x), dy = std::fabs(v - r.y);
          dx = std::min(dx, 1.0f - dx);
          dy = std::min(dy, 1.0f - dy);
          const float e = std::max(dx - r.w, dy - r.h);
          patch = std::max(patch, 1.0f - std::clamp(e / 0.004f, 0.0f, 1.0f));
        }
        const size_t i = static_cast<size_t>(y * n + x) * 4;
        px[i] = u8(fbm(u, v, 4, 5, 11));
        px[i + 1] = u8(fbm(u, v, 3, 5, 23));
        px[i + 2] = u8(fbm(u, v, 16, 4, 37));
        px[i + 3] = u8(patch * 0.35f + 0.65f * patch + (patch > 0 ? 0.0f : fbm(u, v, 8, 2, 41) * 0.6f));
      }
    noise = upload(px, n);
  }
  // --- asphalt (3.5 m tile) -------------------------------------------------
  {
    std::vector<float> h(static_cast<size_t>(n) * n);
    std::vector<float> cav(static_cast<size_t>(n) * n, 1.0f);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float u = static_cast<float>(x) / n, v = static_cast<float>(y) / n;
        // aggregate: per-texel stones (~7 mm) + fine fbm
        const float stone = rnd(x / 2, y / 2, 5);
        const float grain = fbm(u, v, 64, 3, 7);
        float hh = 0.55f * grain + 0.45f * (stone > 0.7f ? stone : stone * 0.5f);
        // hairline cracks: thin ridges of a low-frequency noise
        const float cr = fbm(u, v, 6, 4, 13);
        const float crack = 1.0f - std::clamp(std::fabs(cr - 0.5f) / 0.008f, 0.0f, 1.0f);
        const float crack_mask = std::clamp((fbm(u, v, 3, 2, 17) - 0.55f) * 6.0f, 0.0f, 1.0f);
        const float c = crack * crack_mask;
        hh -= 0.6f * c;
        h[static_cast<size_t>(y * n + x)] = hh;
        cav[static_cast<size_t>(y * n + x)] = 1.0f - 0.7f * c;
        const size_t i = static_cast<size_t>(y * n + x) * 4;
        const float light = stone > 0.93f ? 0.25f : 0.0f;  // occasional light aggregate
        const float t = 0.5f * (0.82f + 0.3f * grain + light) * (1.0f - 0.5f * c);
        px[i] = u8(t);
        px[i + 1] = u8(t * 0.995f);
        px[i + 2] = u8(t * 1.01f);
        px[i + 3] = u8(0.78f + 0.15f * (1.0f - grain) - light * 0.3f);
      }
    asphalt = upload(px, n);
    heightToNormal(h, n, 2.2f, px);
    for (int k = 0; k < n * n; ++k) {
      px[static_cast<size_t>(k) * 4 + 2] = u8(h[static_cast<size_t>(k)]);
      px[static_cast<size_t>(k) * 4 + 3] = u8(cav[static_cast<size_t>(k)]);
    }
    asphaltN = upload(px, n);
  }
  // --- paving: 30 cm tiles, 4x4 per 1.2 m tile, 5 mm joints -----------------
  {
    std::vector<float> h(static_cast<size_t>(n) * n);
    std::vector<float> cav(static_cast<size_t>(n) * n, 1.0f);
    const float tiles = 4.0f;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float u = static_cast<float>(x) / n * tiles, v = static_cast<float>(y) / n * tiles;
        const int tx = static_cast<int>(u), ty = static_cast<int>(v);
        const float fx = u - tx, fy = v - ty;
        const float edge = std::min(std::min(fx, 1.0f - fx), std::min(fy, 1.0f - fy)) * 0.30f;  // metres from joint
        const float joint = 1.0f - std::clamp((edge - 0.0025f) / 0.003f, 0.0f, 1.0f);
        const float bevel = std::clamp(edge / 0.008f, 0.0f, 1.0f);
        const float var = rnd(tx, ty, 71) * 0.14f - 0.07f;
        const float speck = fbm(u / tiles, v / tiles, 96, 2, 73);
        const float dirt = fbm(u / tiles, v / tiles, 4, 3, 79);
        h[static_cast<size_t>(y * n + x)] = bevel * 0.6f + speck * 0.1f;
        cav[static_cast<size_t>(y * n + x)] = 1.0f - 0.6f * joint;
        const float base = (0.30f + var * 0.8f + (speck - 0.5f) * 0.07f) * (1.0f - 0.25f * joint) * (0.92f + 0.12f * dirt);
        const size_t i = static_cast<size_t>(y * n + x) * 4;
        px[i] = u8(base * 1.02f);
        px[i + 1] = u8(base);
        px[i + 2] = u8(base * 0.95f);
        px[i + 3] = u8(0.72f + 0.18f * joint + 0.1f * speck);
      }
    paving = upload(px, n);
    heightToNormal(h, n, 3.0f, px);
    for (int k = 0; k < n * n; ++k) {
      px[static_cast<size_t>(k) * 4 + 2] = u8(h[static_cast<size_t>(k)]);
      px[static_cast<size_t>(k) * 4 + 3] = u8(cav[static_cast<size_t>(k)]);
    }
    pavingN = upload(px, n);
  }
  return noise.id && asphalt.id && asphaltN.id && paving.id && pavingN.id;
}

void DetailTextures::unload() {
  for (Texture2D* t : {&noise, &asphalt, &asphaltN, &paving, &pavingN})
    if (t->id) {
      UnloadTexture(*t);
      *t = Texture2D{};
    }
}

}  // namespace rjc
