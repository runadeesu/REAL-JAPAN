#include "render/trains.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

// Model space while building: x right, y forward, z up (rail top at z = 0).
struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short v() const { return static_cast<unsigned short>(pos.size() / 3); }
  void vert(Vector3 p, Vector3 n, Color c, int mat) {
    pos.insert(pos.end(), {p.x, p.z, -p.y});
    nrm.insert(nrm.end(), {n.x, n.z, -n.y});
    uv.insert(uv.end(), {0.0f, 0.0f});
    col.insert(col.end(), {c.r, c.g, c.b, c.a});
    t2.insert(t2.end(), {static_cast<float>(mat), 0.0f});
  }
  void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col, int mat) {
    const Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a)));
    const unsigned short k = v();
    vert(a, n, col, mat);
    vert(b, n, col, mat);
    vert(c, n, col, mat);
    vert(d, n, col, mat);
    idx.insert(idx.end(), {k, static_cast<unsigned short>(k + 1), static_cast<unsigned short>(k + 2), k,
                           static_cast<unsigned short>(k + 2), static_cast<unsigned short>(k + 3)});
  }
  void box(Vector3 c, Vector3 h, Color col, int mat) {
    const Vector3 p[8] = {{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z - h.z},
                          {c.x - h.x, c.y + h.y, c.z - h.z}, {c.x - h.x, c.y - h.y, c.z + h.z}, {c.x + h.x, c.y - h.y, c.z + h.z},
                          {c.x + h.x, c.y + h.y, c.z + h.z}, {c.x - h.x, c.y + h.y, c.z + h.z}};
    quad(p[4], p[5], p[6], p[7], col, mat);
    quad(p[3], p[2], p[1], p[0], col, mat);
    quad(p[0], p[1], p[5], p[4], col, mat);
    quad(p[2], p[3], p[7], p[6], col, mat);
    quad(p[1], p[2], p[6], p[5], col, mat);
    quad(p[3], p[0], p[4], p[7], col, mat);
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

constexpr int kShell = kMatUntinted, kGlass = 38, kDark = kMatTyre;
constexpr float kFloorAbove = kTrainFloorAbove;

struct Spec {
  float L, W, zFloor, zWin0, zWin1, zSide, zShoulder, zRoof, shoulderX;
  Color body, band, band2;
  float bandZ0, bandZ1;
  bool shinkansen;
};

// One side wall (x = sx * W) along y in [y0, y1], with window openings in the window band.
void sideWall(Geo& g, Geo& glass, const Spec& s, float sx, float y0, float y1, const std::vector<std::pair<float, float>>& windows,
              const std::vector<std::pair<float, float>>& doors) {
  const float x = sx * s.W;
  auto wallq = [&](float ya, float yb, float za, float zb, Color c) {
    if (yb - ya < 1e-3 || zb - za < 1e-3) return;
    if (sx > 0) g.quad({x, ya, za}, {x, yb, za}, {x, yb, zb}, {x, ya, zb}, c, kShell);
    else g.quad({x, yb, za}, {x, ya, za}, {x, ya, zb}, {x, yb, zb}, c, kShell);
  };
  auto bandsq = [&](float ya, float yb, float za, float zb) {
    // split [za, zb] by the colour bands
    float cuts[6] = {za, std::clamp(s.bandZ0, za, zb), std::clamp(s.bandZ1, za, zb), zb, zb, zb};
    wallq(ya, yb, cuts[0], cuts[1], s.body);
    wallq(ya, yb, cuts[1], cuts[2], s.band);
    wallq(ya, yb, cuts[2], cuts[3], s.body);
  };
  // below and above the window band: continuous
  bandsq(y0, y1, s.zFloor, s.zWin0);
  wallq(y0, y1, s.zWin1, s.zSide, s.shinkansen ? s.body : s.band2);
  // window band: piers between openings, doors are full-height panels
  std::vector<std::pair<float, float>> open = windows;
  std::sort(open.begin(), open.end());
  float y = y0;
  for (const auto& w : open) {
    wallq(y, w.first, s.zWin0, s.zWin1, s.body);
    // glass pane slightly inside the opening (drawn for outside views only)
    if (sx > 0) glass.quad({x - 0.03f, w.first, s.zWin0}, {x - 0.03f, w.second, s.zWin0}, {x - 0.03f, w.second, s.zWin1},
                           {x - 0.03f, w.first, s.zWin1}, Color{255, 255, 255, 255}, kGlass);
    else glass.quad({x + 0.03f, w.second, s.zWin0}, {x + 0.03f, w.first, s.zWin0}, {x + 0.03f, w.first, s.zWin1},
                    {x + 0.03f, w.second, s.zWin1}, Color{255, 255, 255, 255}, kGlass);
    y = w.second;
  }
  wallq(y, y1, s.zWin0, s.zWin1, s.body);
  for (const auto& d : doors) {  // door panels just proud of the side, with a dark window
    const float xo = x + sx * 0.012f;
    Color dc{static_cast<unsigned char>(s.body.r * 0.9f), static_cast<unsigned char>(s.body.g * 0.9f),
             static_cast<unsigned char>(s.body.b * 0.9f), 255};
    if (sx > 0) {
      g.quad({xo, d.first, s.zFloor + 0.05f}, {xo, d.second, s.zFloor + 0.05f}, {xo, d.second, s.zWin0}, {xo, d.first, s.zWin0}, dc, kShell);
      glass.quad({xo, d.first + 0.1f, s.zWin0}, {xo, d.second - 0.1f, s.zWin0}, {xo, d.second - 0.1f, s.zWin1}, {xo, d.first + 0.1f, s.zWin1},
                 Color{255, 255, 255, 255}, kGlass);
    } else {
      g.quad({xo, d.second, s.zFloor + 0.05f}, {xo, d.first, s.zFloor + 0.05f}, {xo, d.first, s.zWin0}, {xo, d.second, s.zWin0}, dc, kShell);
      glass.quad({xo, d.second - 0.1f, s.zWin0}, {xo, d.first + 0.1f, s.zWin0}, {xo, d.first + 0.1f, s.zWin1}, {xo, d.second - 0.1f, s.zWin1},
                 Color{255, 255, 255, 255}, kGlass);
    }
  }
}

void roof(Geo& g, const Spec& s, float y0, float y1) {
  const Vector3 a{s.W, 0, s.zSide}, b{s.shoulderX, 0, s.zShoulder}, c{0, 0, s.zRoof};
  const Color rc{static_cast<unsigned char>(s.body.r * 0.85f), static_cast<unsigned char>(s.body.g * 0.85f),
                 static_cast<unsigned char>(s.body.b * 0.85f), 255};
  for (int sx = -1; sx <= 1; sx += 2) {
    auto P = [&](Vector3 p, float y) { return Vector3{p.x * sx, y, p.z}; };
    if (sx > 0) {
      g.quad(P(a, y0), P(a, y1), P(b, y1), P(b, y0), s.body, kShell);
      g.quad(P(b, y0), P(b, y1), P(c, y1), P(c, y0), rc, kShell);
    } else {
      g.quad(P(a, y1), P(a, y0), P(b, y0), P(b, y1), s.body, kShell);
      g.quad(P(b, y1), P(b, y0), P(c, y0), P(c, y1), rc, kShell);
    }
  }
}

void endWall(Geo& g, const Spec& s, float y, float dir, Color c) {
  // flat end (gangway side) as a fan of the cross-section
  const Vector3 prof[] = {{-s.W, y, s.zFloor}, {s.W, y, s.zFloor}, {s.W, y, s.zSide}, {s.shoulderX, y, s.zShoulder},
                          {0, y, s.zRoof}, {-s.shoulderX, y, s.zShoulder}, {-s.W, y, s.zSide}};
  const Vector3 n{0, dir, 0};
  const unsigned short k = g.v();
  for (const auto& p : prof) g.vert(p, n, c, kShell);
  for (unsigned short i = 1; i + 1 < 7; ++i) {
    if (dir > 0) g.idx.insert(g.idx.end(), {k, static_cast<unsigned short>(k + i), static_cast<unsigned short>(k + i + 1)});
    else g.idx.insert(g.idx.end(), {k, static_cast<unsigned short>(k + i + 1), static_cast<unsigned short>(k + i)});
  }
}

void underframe(Geo& g, const Spec& s, float y0, float y1) {
  g.box({0, (y0 + y1) / 2, (s.zFloor + 0.55f) / 2}, {s.W - 0.15f, (y1 - y0) / 2, (s.zFloor - 0.55f) / 2}, Color{50, 52, 56, 255}, kDark);
  for (float yb : {y0 + 2.6f, y1 - 2.6f}) {  // bogies
    g.box({0, yb, 0.5f}, {1.25f, 1.4f, 0.35f}, Color{40, 40, 42, 255}, kDark);
    for (float yw : {yb - 1.05f, yb + 1.05f})
      for (float sx : {-0.78f, 0.78f}) g.box({sx, yw, 0.43f}, {0.08f, 0.43f, 0.43f}, Color{60, 60, 60, 255}, kDark);
  }
}

void interior(Geo& g, const Spec& s, float y0, float y1, const std::vector<std::pair<float, float>>& doors) {
  const float zf = s.zFloor + kFloorAbove;
  g.quad({-s.W + 0.05f, y0, zf}, {s.W - 0.05f, y0, zf}, {s.W - 0.05f, y1, zf}, {-s.W + 0.05f, y1, zf}, Color{120, 110, 100, 255}, kShell);
  g.quad({-s.W + 0.05f, y1, s.zSide - 0.1f}, {s.W - 0.05f, y1, s.zSide - 0.1f}, {s.W - 0.05f, y0, s.zSide - 0.1f}, {-s.W + 0.05f, y0, s.zSide - 0.1f},
         Color{225, 225, 222, 255}, kShell);  // ceiling
  for (float x : {-0.5f, 0.5f})  // light strips
    g.quad({x - 0.08f, y1 - 0.5f, s.zSide - 0.12f}, {x + 0.08f, y1 - 0.5f, s.zSide - 0.12f}, {x + 0.08f, y0 + 0.5f, s.zSide - 0.12f},
           {x - 0.08f, y0 + 0.5f, s.zSide - 0.12f}, Color{255, 255, 255, 255}, kGlass);
  if (s.shinkansen) {  // rows of 2+3 seats facing forward
    for (float y = y0 + 1.5f; y < y1 - 1.0f; y += kShinkansenSeatPitch) {
      for (float x : {-1.35f, -0.9f, 0.35f, 0.82f, 1.29f}) {
        g.box({x, y, zf + 0.25f}, {0.22f, 0.25f, 0.23f}, Color{40, 60, 120, 255}, kShell);
        g.box({x, y - 0.26f, zf + 0.7f}, {0.22f, 0.06f, 0.45f}, Color{40, 60, 120, 255}, kShell);
      }
    }
  } else {  // long bench seats along the sides between the doors, grab poles
    std::vector<float> cuts = {y0 + 0.4f};
    for (const auto& d : doors) cuts.insert(cuts.end(), {d.first - 0.15f, d.second + 0.15f});
    cuts.push_back(y1 - 0.4f);
    for (size_t k = 0; k + 1 < cuts.size(); k += 2) {
      const float a = cuts[k], b = cuts[k + 1];
      if (b - a < 0.6f) continue;
      for (float sx : {-1.0f, 1.0f}) {
        g.box({sx * (s.W - 0.32f), (a + b) / 2, zf + 0.22f}, {0.26f, (b - a) / 2, 0.2f}, Color{60, 110, 90, 255}, kShell);
        g.box({sx * (s.W - 0.08f), (a + b) / 2, zf + 0.72f}, {0.05f, (b - a) / 2, 0.3f}, Color{60, 110, 90, 255}, kShell);  // backrest
      }
    }
    for (float y = y0 + 3.0f; y < y1 - 2.0f; y += 3.5f) g.box({0.6f, y, zf + 1.1f}, {0.02f, 0.02f, 1.1f}, Color{200, 200, 200, 255}, kMatMetal);
  }
}

TrainCarModel makeCommuter(bool cab) {
  Spec s{20.0f, 1.45f, 1.1f, 1.95f, 2.8f, 2.95f, 3.38f, 3.62f, 1.18f, {196, 198, 202, 255}, {28, 150, 128, 255}, {196, 198, 202, 255},
         1.36f, 1.5f, false};
  Geo g, glass, in;
  const float y0 = -s.L / 2, y1 = s.L / 2;
  std::vector<std::pair<float, float>> doors, windows;
  for (int k = 0; k < 4; ++k) {
    const float yc = y0 + 2.45f + k * 5.03f;
    doors.push_back({yc - 0.65f, yc + 0.65f});
  }
  for (int k = 0; k < 3; ++k) {
    const float a = doors[static_cast<size_t>(k)].second + 0.35f, b = doors[static_cast<size_t>(k) + 1].first - 0.35f;
    windows.push_back({a, (a + b) / 2 - 0.08f});
    windows.push_back({(a + b) / 2 + 0.08f, b});
  }
  // door openings are not window holes: exclude them from the pier logic by treating door spans as piers
  for (int sx = -1; sx <= 1; sx += 2) sideWall(g, glass, s, static_cast<float>(sx), y0, y1, windows, doors);
  roof(g, s, y0, y1);
  endWall(g, s, y0, -1, s.body);
  if (cab) {  // front face: sloped dark glass, band, headlights
    endWall(g, s, y1, 1, Color{40, 42, 46, 255});
    glass.quad({-1.3f, y1 + 0.02f, 1.9f}, {1.3f, y1 + 0.02f, 1.9f}, {1.3f, y1 + 0.02f, 2.85f}, {-1.3f, y1 + 0.02f, 2.85f}, Color{255, 255, 255, 255}, kGlass);
    g.box({0, y1 + 0.03f, 1.43f}, {1.44f, 0.02f, 0.07f}, s.band, kShell);
    for (float x : {-1.0f, 1.0f}) g.box({x, y1 + 0.04f, 1.2f}, {0.18f, 0.02f, 0.08f}, Color{255, 250, 235, 255}, kMatSignalLamp);
  } else {
    endWall(g, s, y1, 1, s.body);
  }
  underframe(g, s, y0, y1);
  // pantograph frame on the roof
  g.box({0, 2.0f, s.zRoof + 0.15f}, {0.9f, 0.6f, 0.03f}, Color{80, 80, 80, 255}, kMatMetal);
  interior(in, s, y0, y1, doors);
  TrainCarModel m;
  m.length = s.L;
  m.shell = g.upload();
  m.glass = glass.upload();
  m.interior = in.upload();
  return m;
}

TrainCarModel makeShinkansen(bool nose) {
  Spec s{25.0f, 1.69f, 1.1f, 2.05f, 2.55f, 3.1f, 3.48f, 3.65f, 1.3f, {242, 242, 240, 255}, {26, 64, 160, 255}, {242, 242, 240, 255},
         1.38f, 1.55f, true};
  Geo g, glass, in;
  const float y0 = -s.L / 2;
  const float y1 = nose ? s.L / 2 - 11.0f : s.L / 2;
  std::vector<std::pair<float, float>> windows, doors = {{y0 + 0.6f, y0 + 1.6f}};
  for (float y = y0 + 2.4f; y < y1 - 0.9f; y += 1.04f) windows.push_back({y, y + 0.62f});
  for (int sx = -1; sx <= 1; sx += 2) sideWall(g, glass, s, static_cast<float>(sx), y0, y1, windows, doors);
  roof(g, s, y0, y1);
  endWall(g, s, y0, -1, s.body);
  if (!nose) {
    endWall(g, s, y1, 1, s.body);
  } else {
    // long nose: cross-section shrinks and drops towards the tip
    const int nr = 12;
    const float Ln = s.L / 2 - y1;
    std::vector<std::vector<Vector3>> rings;
    for (int i = 0; i <= nr; ++i) {
      const float t = static_cast<float>(i) / nr;
      const float y = y1 + t * Ln;
      const float w = s.W * (1.0f - 0.72f * std::pow(t, 1.9f));
      const float zt = s.zRoof - 2.25f * std::pow(t, 1.35f);
      const float zb = s.zFloor - 0.15f + 0.2f * t;
      const float zs = std::min(zt - 0.1f, s.zSide - 1.45f * std::pow(t, 1.3f));
      rings.push_back({{-w, y, zb}, {w, y, zb}, {w, y, std::max(zb + 0.1f, zs)}, {w * 0.72f, y, std::max(zb + 0.12f, (zs + zt) / 2)},
                       {0, y, zt}, {-w * 0.72f, y, std::max(zb + 0.12f, (zs + zt) / 2)}, {-w, y, std::max(zb + 0.1f, zs)}});
    }
    for (int i = 0; i < nr; ++i) {
      const float t = (i + 0.5f) / nr;
      for (int k = 0; k < 7; ++k) {
        const Vector3 a = rings[static_cast<size_t>(i)][static_cast<size_t>(k)], b = rings[static_cast<size_t>(i)][static_cast<size_t>((k + 1) % 7)];
        const Vector3 c = rings[static_cast<size_t>(i) + 1][static_cast<size_t>((k + 1) % 7)], d = rings[static_cast<size_t>(i) + 1][static_cast<size_t>(k)];
        const bool top = k == 3 || k == 4;
        const bool cabwin = top && t > 0.42f && t < 0.62f;
        const bool bandSide = (k == 1 || k == 5 + 1 - 1) && t < 0.7f;
        Color c0 = cabwin ? Color{20, 22, 26, 255} : (bandSide ? s.band : s.body);
        if (k == 0) continue;  // bottom closed by the underframe
        g.quad(a, d, c, b, c0, cabwin ? kGlass : kShell);
      }
    }
    g.box({0, y1 + Ln - 0.2f, 1.05f}, {0.2f, 0.2f, 0.15f}, Color{255, 250, 235, 255}, kMatSignalLamp);
  }
  underframe(g, s, y0, nose ? y1 + 4.0f : y1);
  interior(in, s, y0, y1, doors);
  TrainCarModel m;
  m.length = s.L;
  m.shell = g.upload();
  m.glass = glass.upload();
  m.interior = in.upload();
  return m;
}

}  // namespace

void TrainModels::build() {
  if (ready_) return;
  m_[static_cast<int>(TrainCar::CommuterMid)] = makeCommuter(false);
  m_[static_cast<int>(TrainCar::CommuterCab)] = makeCommuter(true);
  m_[static_cast<int>(TrainCar::ShinkansenMid)] = makeShinkansen(false);
  m_[static_cast<int>(TrainCar::ShinkansenNose)] = makeShinkansen(true);
  ready_ = true;
}

void TrainModels::unload() {
  if (!ready_) return;
  for (auto& m : m_)
    for (Mesh* x : {&m.shell, &m.glass, &m.interior})
      if (x->vaoId) UnloadMesh(*x);
  ready_ = false;
}

}  // namespace rjc
