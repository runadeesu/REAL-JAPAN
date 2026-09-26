#include "render/foliage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "raymath.h"
#include "world/detail.hpp"

namespace rjc {
namespace {

struct Rng {
  uint32_t s;
  float next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 16777215.0f;
  }
  float range(float a, float b) { return a + (b - a) * next(); }
};

struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short v() const { return static_cast<unsigned short>(pos.size() / 3); }
  void vert(Vector3 p, Vector3 n, Vector2 t, Color c, float mat, float param) {
    pos.insert(pos.end(), {p.x, p.y, p.z});
    nrm.insert(nrm.end(), {n.x, n.y, n.z});
    uv.insert(uv.end(), {t.x, t.y});
    col.insert(col.end(), {c.r, c.g, c.b, c.a});
    t2.insert(t2.end(), {mat, param});
  }
  Mesh upload() {
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    if (m.vertexCount == 0) return m;
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.texcoords2 = t2.data();
    m.colors = col.data();
    m.indices = idx.data();
    UploadMesh(&m, false);
    m.vertices = m.normals = m.texcoords = m.texcoords2 = nullptr;
    m.colors = nullptr;
    m.indices = nullptr;
    return m;
  }
};

// Tapered cylinder from a to b.
void limb(Geo& g, Vector3 a, Vector3 b, float ra, float rb, Color c, int seg) {
  const Vector3 axis = Vector3Normalize(Vector3Subtract(b, a));
  Vector3 side = std::fabs(axis.y) < 0.9f ? Vector3CrossProduct(axis, Vector3{0, 1, 0}) : Vector3CrossProduct(axis, Vector3{1, 0, 0});
  side = Vector3Normalize(side);
  const Vector3 up = Vector3CrossProduct(side, axis);
  const unsigned short base = g.v();
  for (int i = 0; i <= seg; ++i) {
    const float t = static_cast<float>(i) / seg * 2.0f * PI;
    const Vector3 n = Vector3Add(Vector3Scale(side, std::cos(t)), Vector3Scale(up, std::sin(t)));
    g.vert(Vector3Add(a, Vector3Scale(n, ra)), n, {static_cast<float>(i) / seg, 0}, c, kMatBark, 0.0f);
    g.vert(Vector3Add(b, Vector3Scale(n, rb)), n, {static_cast<float>(i) / seg, 1}, c, kMatBark, 0.0f);
  }
  for (int i = 0; i < seg; ++i) {
    const auto k = static_cast<unsigned short>(base + i * 2);
    g.idx.insert(g.idx.end(), {k, static_cast<unsigned short>(k + 2), static_cast<unsigned short>(k + 1), static_cast<unsigned short>(k + 1),
                               static_cast<unsigned short>(k + 2), static_cast<unsigned short>(k + 3)});
  }
}

}  // namespace

TreeMeshes buildTree(unsigned seed, float h, float r) {
  Rng rng{seed * 2654435761u + 12345u};
  Geo bark, leaf;
  const Color barkc{static_cast<unsigned char>(78 + 20 * rng.next()), static_cast<unsigned char>(66 + 14 * rng.next()), 56, 255};
  const float trunk_h = h * rng.range(0.32f, 0.42f);
  const float tr = std::clamp(0.08f + h * 0.012f, 0.1f, 0.3f);
  const Vector3 top{0, trunk_h, 0};
  limb(bark, {0, 0, 0}, top, tr, tr * 0.8f, barkc, 10);
  // Crown: slightly flattened ellipsoid centred in the upper part of the tree.
  const float cr = std::max(r, h * 0.2f);
  const float cy = trunk_h + (h - trunk_h) * 0.52f;
  const float ry = (h - trunk_h) * 0.55f;
  const int nb = 5 + static_cast<int>(rng.next() * 3);
  std::vector<Vector3> tips;
  for (int i = 0; i < nb; ++i) {
    const float az = (i + rng.next() * 0.6f) / nb * 2.0f * PI;
    const float el = rng.range(0.5f, 0.95f);
    const Vector3 dir = Vector3Normalize(Vector3{std::cos(az) * std::cos(el), std::sin(el) * 1.3f, std::sin(az) * std::cos(el)});
    const float len = (h - trunk_h) * rng.range(0.45f, 0.7f);
    const Vector3 a{0, trunk_h * rng.range(0.85f, 1.0f), 0};
    const Vector3 b = Vector3Add(a, Vector3Scale(dir, len));
    limb(bark, a, b, tr * 0.55f, tr * 0.18f, barkc, 6);
    tips.push_back(b);
    for (int k = 0; k < 2; ++k) {  // secondary branches
      const Vector3 m = Vector3Lerp(a, b, rng.range(0.45f, 0.8f));
      const Vector3 d2 = Vector3Normalize(Vector3Add(dir, Vector3{rng.range(-0.8f, 0.8f), rng.range(0.1f, 0.6f), rng.range(-0.8f, 0.8f)}));
      const Vector3 e = Vector3Add(m, Vector3Scale(d2, len * 0.45f));
      limb(bark, m, e, tr * 0.2f, tr * 0.06f, barkc, 5);
      tips.push_back(e);
    }
  }
  // Leaf cards clustered around branch tips and filling the crown volume.
  const int ncards = std::clamp(static_cast<int>(cr * cr * ry * 9.0f), 120, 420);
  const Vector3 cc{0, cy, 0};
  for (int i = 0; i < ncards; ++i) {
    Vector3 p;
    if (i % 3 != 0 && !tips.empty()) {
      const Vector3& t = tips[static_cast<size_t>(rng.next() * (tips.size() - 0.001f))];
      p = Vector3Add(t, Vector3{rng.range(-1, 1) * cr * 0.45f, rng.range(-0.6f, 0.8f) * ry * 0.4f, rng.range(-1, 1) * cr * 0.45f});
    } else {
      float x, y, z;
      do {
        x = rng.range(-1, 1);
        y = rng.range(-1, 1);
        z = rng.range(-1, 1);
      } while (x * x + y * y + z * z > 1.0f);
      p = Vector3Add(cc, Vector3{x * cr, y * ry, z * cr});
    }
    const float s = rng.range(0.7f, 1.25f) * std::clamp(cr * 0.45f, 0.6f, 1.2f);
    const float yaw = rng.range(0, 2 * PI), tilt = rng.range(-0.9f, 0.9f);
    const Vector3 u{std::cos(yaw) * s, 0, std::sin(yaw) * s};
    const Vector3 w{-std::sin(yaw) * std::sin(tilt) * s, std::cos(tilt) * s, std::cos(yaw) * std::sin(tilt) * s};
    Vector3 n = Vector3Normalize(Vector3Subtract(p, cc));  // volumetric crown shading
    n = Vector3Normalize(Vector3Add(n, Vector3{0, 0.35f, 0}));
    const float sway = std::clamp((p.y - trunk_h) / (h - trunk_h + 0.01f), 0.0f, 1.0f);
    const unsigned char tint = static_cast<unsigned char>(200 + 55 * rng.next());
    const Color lc{tint, 255, static_cast<unsigned char>(tint * 0.85f), 255};
    const float q = rng.next() < 0.5f ? 0.0f : 0.5f;  // pick one of two leaf clusters in the atlas
    const unsigned short b0 = leaf.v();
    leaf.vert(Vector3Subtract(Vector3Subtract(p, u), w), n, {q, 1}, lc, kMatFoliage, sway);
    leaf.vert(Vector3Subtract(Vector3Add(p, u), w), n, {q + 0.5f, 1}, lc, kMatFoliage, sway);
    leaf.vert(Vector3Add(Vector3Add(p, u), w), n, {q + 0.5f, 0}, lc, kMatFoliage, sway);
    leaf.vert(Vector3Add(Vector3Subtract(p, u), w), n, {q, 0}, lc, kMatFoliage, sway);
    leaf.idx.insert(leaf.idx.end(), {b0, static_cast<unsigned short>(b0 + 1), static_cast<unsigned short>(b0 + 2), b0,
                                     static_cast<unsigned short>(b0 + 2), static_cast<unsigned short>(b0 + 3)});
  }
  // Built y-up; convert to ENU (x east, y north, z up) like all cell-local detail geometry.
  for (Geo* g : {&bark, &leaf}) {
    for (size_t i = 0; i + 2 < g->pos.size(); i += 3) {
      const float y = g->pos[i + 1], z = g->pos[i + 2];
      g->pos[i + 1] = -z;
      g->pos[i + 2] = y;
      const float ny = g->nrm[i + 1], nz = g->nrm[i + 2];
      g->nrm[i + 1] = -nz;
      g->nrm[i + 2] = ny;
    }
  }
  TreeMeshes t;
  t.bark = bark.upload();
  t.leaves = leaf.upload();
  return t;
}

Mesh buildHedge(const float* tris, int ntris, float hh) {
  Geo g;
  auto rl = [](const float* p, float dz) { return Vector3{p[0], p[1], p[2] + dz}; };  // cell ENU
  const Color c{230, 255, 220, 255};
  for (int t = 0; t < ntris; ++t) {
    const float* a = tris + t * 9;
    const unsigned short b0 = g.v();
    for (int k = 0; k < 3; ++k) {
      const float* p = a + k * 3;
      g.vert(rl(p, hh), {0, 0, 1}, {p[0] * 0.8f, p[1] * 0.8f}, c, kMatFoliage, 0.15f);
    }
    g.idx.insert(g.idx.end(), {b0, static_cast<unsigned short>(b0 + 1), static_cast<unsigned short>(b0 + 2)});
    // sides of each triangle edge (interior edges are hidden inside the hedge)
    for (int e = 0; e < 3; ++e) {
      const float* p = a + e * 3;
      const float* q = a + ((e + 1) % 3) * 3;
      const Vector3 d = Vector3Normalize(Vector3{q[0] - p[0], q[1] - p[1], 0});
      const Vector3 n{d.y, -d.x, 0};
      const float L = std::hypot(q[0] - p[0], q[1] - p[1]);
      const unsigned short s0 = g.v();
      g.vert(rl(p, 0.0f), n, {0, 1}, c, kMatFoliage, 0.0f);
      g.vert(rl(q, 0.0f), n, {L * 0.8f, 1}, c, kMatFoliage, 0.0f);
      g.vert(rl(q, hh), n, {L * 0.8f, 1 - hh * 0.8f}, c, kMatFoliage, 0.15f);
      g.vert(rl(p, hh), n, {0, 1 - hh * 0.8f}, c, kMatFoliage, 0.15f);
      g.idx.insert(g.idx.end(), {s0, static_cast<unsigned short>(s0 + 1), static_cast<unsigned short>(s0 + 2), s0,
                                 static_cast<unsigned short>(s0 + 2), static_cast<unsigned short>(s0 + 3)});
      if (g.v() > 60000) break;
    }
    if (g.v() > 60000) break;
  }
  return g.upload();
}

Texture2D generateLeafTexture(int n) {
  // Two leaf-cluster sprites side by side (u 0..0.5 and 0.5..1): many small elliptical leaves.
  std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4, 0);
  Rng rng{987654321u};
  for (int half = 0; half < 2; ++half) {
    const float x0 = half * n * 0.5f, w = n * 0.5f;
    for (int i = 0; i < 260; ++i) {
      // position inside a rough disc (clusters are denser in the middle)
      float dx, dy;
      do {
        dx = rng.range(-1, 1);
        dy = rng.range(-1, 1);
      } while (dx * dx + dy * dy > 1.0f);
      const float cx = x0 + w * (0.5f + dx * 0.42f), cy = n * (0.5f + dy * 0.42f);
      const float len = rng.range(0.035f, 0.06f) * n, wid = len * rng.range(0.38f, 0.5f);
      const float ang = rng.range(0, 2 * PI);
      const float ca = std::cos(ang), sa = std::sin(ang);
      const float shade = rng.range(0.55f, 1.0f);
      const float hue = rng.next();
      const int r0 = static_cast<int>(std::max(0.0f, cx - len)), r1 = static_cast<int>(std::min(x0 + w - 1, cx + len));
      const int c0 = static_cast<int>(std::max(0.0f, cy - len)), c1 = static_cast<int>(std::min(n - 1.0f, cy + len));
      for (int y = c0; y <= c1; ++y)
        for (int x = r0; x <= r1; ++x) {
          const float lx = ((x - cx) * ca + (y - cy) * sa) / len, ly = (-(x - cx) * sa + (y - cy) * ca) / wid;
          const float d = lx * lx + ly * ly;
          if (d > 1.0f) continue;
          const float vein = std::fabs(ly) < 0.08f ? 0.85f : 1.0f;
          const float edge = 0.8f + 0.2f * (1.0f - d);
          const size_t k = (static_cast<size_t>(y) * n + x) * 4;
          // greens from yellowish to dark (linear albedo ~0.05-0.18 after the shader's decode)
          const float g = (0.38f + 0.22f * hue) * shade * vein * edge;
          px[k] = static_cast<unsigned char>(255 * std::min(1.0f, g * (0.55f + 0.25f * hue)));
          px[k + 1] = static_cast<unsigned char>(255 * std::min(1.0f, g));
          px[k + 2] = static_cast<unsigned char>(255 * std::min(1.0f, g * 0.35f));
          px[k + 3] = 255;
        }
    }
  }
  Image img{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  Texture2D t = LoadTextureFromImage(img);
  GenTextureMipmaps(&t);
  SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  return t;
}

}  // namespace rjc
