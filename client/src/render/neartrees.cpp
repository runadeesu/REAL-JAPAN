#include "render/neartrees.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "render/renderer.hpp"
#include "world/coords.hpp"
#include "world/detail.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

constexpr float kTile = 40.0f;
constexpr float kSpacing = 5.0f;
constexpr float kPi = 3.14159265358979f;

uint32_t hashU(int x, int y, uint32_t salt) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + salt * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float hashF(int x, int y, uint32_t salt) { return static_cast<float>(hashU(x, y, salt) & 0xffffff) / 16777215.0f; }

float valueNoise(float x, float y, uint32_t salt) {
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const float fx = x - x0, fy = y - y0;
  const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const float a = hashF(x0, y0, salt), b = hashF(x0 + 1, y0, salt), c = hashF(x0, y0 + 1, salt), d = hashF(x0 + 1, y0 + 1, salt);
  return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short v() const { return static_cast<unsigned short>(pos.size() / 3); }
  // (x east, y north, z up) in origin ENU -> raylib (x, z, -y)
  void vert(float x, float y, float z, float nx, float ny, float nz, const unsigned char c[4], int mat) {
    pos.insert(pos.end(), {x, z, -y});
    nrm.insert(nrm.end(), {nx, nz, -ny});
    uv.insert(uv.end(), {0.0f, 0.0f});
    col.insert(col.end(), {c[0], c[1], c[2], c[3]});
    t2.insert(t2.end(), {static_cast<float>(mat), 0.0f});
  }
  Mesh upload() {
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    if (!m.vertexCount) return m;
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.texcoords2 = t2.data();
    m.colors = col.data();
    m.indices = idx.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    return m;
  }
};

// A cone (or frustum) of n sides about the vertical axis at (x, y): radius r0 at z0 to r1 at z1.
void frustum(Geo& g, float x, float y, float z0, float z1, float r0, float r1, int n, float rot, const unsigned char c[4], int mat) {
  if (g.v() > 65000) return;
  const unsigned short k = g.v();
  const float slope = (r0 - r1) / std::max(0.01f, z1 - z0);
  for (int i = 0; i <= n; ++i) {
    const float a = rot + 2.0f * kPi * i / n, ca = std::cos(a), sa = std::sin(a);
    const float l = std::sqrt(1.0f + slope * slope);
    g.vert(x + ca * r0, y + sa * r0, z0, ca / l, sa / l, slope / l, c, mat);
    g.vert(x + ca * r1, y + sa * r1, z1, ca / l, sa / l, slope / l, c, mat);
  }
  for (int i = 0; i < n; ++i) {
    const unsigned short a = static_cast<unsigned short>(k + i * 2), b = static_cast<unsigned short>(a + 1), cc = static_cast<unsigned short>(a + 2),
                         d = static_cast<unsigned short>(a + 3);
    g.idx.insert(g.idx.end(), {a, cc, d, a, d, b});
  }
}

// A lumpy spheroid crown: rings of n points between z0 and z1, radius r at the widest.
void spheroid(Geo& g, float x, float y, float z0, float z1, float r, int n, uint32_t seed, const unsigned char c[4], int mat) {
  if (g.v() > 65000) return;
  const int rings = 4;
  const unsigned short k = g.v();
  const float zc = (z0 + z1) * 0.5f, hz = (z1 - z0) * 0.5f;
  for (int j = 0; j <= rings; ++j) {
    const float t = kPi * j / rings;  // 0 at the top
    const float rz = std::cos(t), rr = std::sin(t);
    for (int i = 0; i <= n; ++i) {
      const float a = 2.0f * kPi * i / n;
      const float bump = 0.85f + 0.3f * hashF(i % n, j, seed);
      const float ca = std::cos(a), sa = std::sin(a);
      g.vert(x + ca * rr * r * bump, y + sa * rr * r * bump, zc + rz * hz * (j == 0 || j == rings ? 1.0f : bump), ca * rr, sa * rr, rz, c, mat);
    }
  }
  for (int j = 0; j < rings; ++j)
    for (int i = 0; i < n; ++i) {
      const unsigned short a = static_cast<unsigned short>(k + j * (n + 1) + i), b = static_cast<unsigned short>(a + 1),
                           d = static_cast<unsigned short>(a + n + 1), e = static_cast<unsigned short>(d + 1);
      g.idx.insert(g.idx.end(), {a, d, e, a, e, b});
    }
}

}  // namespace

void NearTrees::clear() {
  for (auto& [k, t] : tiles_) {
    if (t.crowns.vaoId) UnloadMesh(t.crowns);
    if (t.trunks.vaoId) UnloadMesh(t.trunks);
  }
  tiles_.clear();
  trees_ = 0;
}

NearTrees::Tile NearTrees::build(const World& world, int tx, int ty) const {
  Tile T;
  Geo crowns, trunks;
  const float x0 = tx * kTile, y0 = ty * kTile;
  // stable planting across origin rebases: a global grid from the geodetic position of the tile
  const rj::geo::Geodetic g0 = world.toGeodetic({x0, y0, 0.0});
  const double gx0 = g0.lon_deg * 111320.0 * std::cos(g0.lat_deg * kPi / 180.0), gy0 = g0.lat_deg * 110574.0;
  const int ix0 = static_cast<int>(std::floor(gx0 / kSpacing)), iy0 = static_cast<int>(std::floor(gy0 / kSpacing));
  const float ox = static_cast<float>(ix0 * kSpacing - gx0), oy = static_cast<float>(iy0 * kSpacing - gy0);
  const int n = static_cast<int>(kTile / kSpacing) + 1;
  for (int a = 0; a <= n; ++a)
    for (int b = 0; b <= n; ++b) {
      const int gi = ix0 + b, gj = iy0 + a;
      const float x = x0 + ox + b * kSpacing + (hashF(gi, gj, 1) - 0.5f) * kSpacing * 0.8f;
      const float y = y0 + oy + a * kSpacing + (hashF(gi, gj, 2) - 0.5f) * kSpacing * 0.8f;
      if (x < x0 || x >= x0 + kTile || y < y0 || y >= y0 + kTile) continue;
      if (hashF(gi, gj, 3) < 0.12f) continue;  // gaps between the crowns
      // the loaded cell under the point: forest there, and how tall the canopy stands
      float h = -1.0f;
      for (const auto& [code, c] : world.cells()) {
        const CellCpu& cpu = *c->cpu;
        const int nx = cpu.tnx, ny = cpu.tny;
        if (cpu.forest_mask.empty() || nx < 2 || ny < 2) continue;
        const double vx = x - c->p00.x, vy = y - c->p00.y;
        const double det = c->ex.x * c->ey.y - c->ex.y * c->ey.x;
        if (std::abs(det) < 1e-9) continue;
        const double fj = (vx * c->ey.y - vy * c->ey.x) / det, fi = (c->ex.x * vy - c->ex.y * vx) / det;
        if (fi < 0 || fj < 0 || fi > ny - 1 || fj > nx - 1) continue;
        const int ma = static_cast<int>(std::lround(fi / (ny - 1) * 256.0)), mb = static_cast<int>(std::lround(fj / (nx - 1) * 256.0));
        if (!cpu.forest_mask[static_cast<size_t>(std::clamp(ma, 0, 256) * 257 + std::clamp(mb, 0, 256))]) break;
        const int i = std::min(static_cast<int>(fi), ny - 2), j = std::min(static_cast<int>(fj), nx - 2);
        const float u = static_cast<float>(fi - i), w = static_cast<float>(fj - j);
        auto H = [&](int r, int q) { return cpu.canopy_h[static_cast<size_t>(r * nx + q)]; };
        h = (H(i, j) * (1 - w) + H(i, j + 1) * w) * (1 - u) + (H(i + 1, j) * (1 - w) + H(i + 1, j + 1) * w) * u;
        break;
      }
      if (h < 4.0f) continue;
      const auto gz = world.terrainHeight(x, y);
      if (!gz) continue;
      const float z = static_cast<float>(*gz) - 0.2f;
      h *= 0.82f + 0.3f * hashF(gi, gj, 4);
      // conifer plantations and broadleaf patches (about 180 m across)
      const bool broad = valueNoise(static_cast<float>(gx0 + b * kSpacing) / 180.0f, static_cast<float>(gy0 + a * kSpacing) / 180.0f, 7) > 0.56f;
      const unsigned char tone = static_cast<unsigned char>(108 + 40 * hashF(gi, gj, 5));
      const unsigned char cc[4] = {tone, 128, static_cast<unsigned char>(broad ? 255 : 0), 255};
      const unsigned char bark[4] = {static_cast<unsigned char>(broad ? 88 : 104), static_cast<unsigned char>(broad ? 80 : 74), static_cast<unsigned char>(broad ? 72 : 58), 255};
      const float rot = hashF(gi, gj, 6) * 6.28f;
      if (broad) {
        const float r = h * (0.2f + 0.08f * hashF(gi, gj, 8));
        frustum(trunks, x, y, z, z + h * 0.55f, 0.22f + h * 0.008f, 0.12f, 5, rot, bark, kMatUntinted);
        spheroid(crowns, x, y, z + h * 0.32f, z + h, r, 7, hashU(gi, gj, 9), cc, kMatCanopy);
      } else {
        const float r = h * (0.13f + 0.04f * hashF(gi, gj, 8));
        frustum(trunks, x, y, z, z + h * 0.45f, 0.2f + h * 0.009f, 0.1f, 5, rot, bark, kMatUntinted);
        frustum(crowns, x, y, z + h * 0.28f, z + h * 0.72f, r, r * 0.55f, 7, rot, cc, kMatCanopy);
        frustum(crowns, x, y, z + h * 0.6f, z + h, r * 0.72f, 0.05f, 7, rot + 0.4f, cc, kMatCanopy);
      }
      ++T.trees;
    }
  T.crowns = crowns.upload();
  T.trunks = trunks.upload();
  return T;
}

void NearTrees::update(const World& world, const rj::geo::Vec3d& cam) {
  if (!world.hasOrigin()) return;
  // (tiles are built in origin ENU: after a rebase they are rebuilt)
  const rj::geo::Geodetic o = world.toGeodetic({0.0, 0.0, 0.0});
  if (!have_origin_ || std::fabs(o.lat_deg - origin_.lat_deg) > 1e-9 || std::fabs(o.lon_deg - origin_.lon_deg) > 1e-9) {
    clear();
    origin_ = o;
    have_origin_ = true;
  }
  const int cx = static_cast<int>(std::floor(cam.x / kTile)), cy = static_cast<int>(std::floor(cam.y / kTile));
  const int reach = static_cast<int>(std::ceil(kRadius / kTile)) + 1;
  auto key = [](int x, int y) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(y); };
  int built = 0;
  for (int dy = -reach; dy <= reach; ++dy)
    for (int dx = -reach; dx <= reach; ++dx) {
      const int tx = cx + dx, ty = cy + dy;
      const float nx = std::clamp(static_cast<float>(cam.x), tx * kTile, (tx + 1) * kTile);
      const float ny = std::clamp(static_cast<float>(cam.y), ty * kTile, (ty + 1) * kTile);
      if (std::hypot(nx - cam.x, ny - cam.y) > kRadius) continue;
      if (tiles_.count(key(tx, ty)) || built >= 4) continue;  // (a few new tiles a frame)
      tiles_[key(tx, ty)] = build(world, tx, ty);
      ++built;
    }
  // drop the tiles left behind
  trees_ = 0;
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    const int tx = static_cast<int>(it->first >> 32), ty = static_cast<int>(static_cast<int32_t>(it->first & 0xffffffff));
    const double mx = (tx + 0.5) * kTile - cam.x, my = (ty + 0.5) * kTile - cam.y;
    if (std::hypot(mx, my) > kRadius + kTile * 2.0) {
      if (it->second.crowns.vaoId) UnloadMesh(it->second.crowns);
      if (it->second.trunks.vaoId) UnloadMesh(it->second.trunks);
      it = tiles_.erase(it);
    } else {
      trees_ += it->second.trees;
      ++it;
    }
  }
}

void NearTrees::draw(Renderer& r) const {
  const Matrix I = MatrixIdentity();
  for (const auto& [k, t] : tiles_) {
    if (t.crowns.vaoId) r.drawMeshMat(t.crowns, I, -1, WHITE);
    if (t.trunks.vaoId) r.drawMeshMat(t.trunks, I, -1, WHITE);
  }
}

}  // namespace rjc
