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

// Inner wall panels with the window and door openings (seen when riding).
void innerWalls(Geo& g, const Spec& s, float y0, float y1, const std::vector<std::pair<float, float>>& windows,
                const std::vector<std::pair<float, float>>& doors, Color c) {
  const float zf = s.zFloor + kFloorAbove, x = s.W - 0.045f, door_top = s.zWin1 + 0.12f;
  auto band = [&](float za, float zb, std::vector<std::pair<float, float>> open) {
    std::sort(open.begin(), open.end());
    float y = y0 + 0.05f;
    std::vector<std::pair<float, float>> solid;
    for (const auto& o : open) {
      if (o.first > y) solid.push_back({y, o.first});
      y = std::max(y, o.second);
    }
    if (y < y1 - 0.05f) solid.push_back({y, y1 - 0.05f});
    for (const auto& q : solid)
      for (float sx : {-1.0f, 1.0f}) {
        const float xx = sx * x;
        if (sx > 0) g.quad({xx, q.second, za}, {xx, q.first, za}, {xx, q.first, zb}, {xx, q.second, zb}, c, kShell);
        else g.quad({xx, q.first, za}, {xx, q.second, za}, {xx, q.second, zb}, {xx, q.first, zb}, c, kShell);
      }
  };
  std::vector<std::pair<float, float>> wd = windows;
  wd.insert(wd.end(), doors.begin(), doors.end());
  band(zf, s.zWin0, doors);
  band(s.zWin0, s.zWin1, wd);
  band(s.zWin1, door_top, doors);
  band(door_top, s.zSide - 0.1f, {});
  // window reveals (the wall thickness round each opening)
  const Color rev{206, 206, 202, 255};
  for (const auto& w : windows)
    for (float sx : {-1.0f, 1.0f}) {
      const float xi = sx * x, xo = sx * (s.W - 0.005f);
      g.quad({xi, w.first, s.zWin0}, {xi, w.second, s.zWin0}, {xo, w.second, s.zWin0}, {xo, w.first, s.zWin0}, rev, kShell);
      g.quad({xi, w.second, s.zWin1}, {xi, w.first, s.zWin1}, {xo, w.first, s.zWin1}, {xo, w.second, s.zWin1}, rev, kShell);
    }
  // door leaves from inside: stainless with a (dark) window
  for (const auto& d : doors)
    for (float sx : {-1.0f, 1.0f}) {
      const float xx = sx * (x - 0.01f);
      const Color st{176, 178, 182, 255};
      g.box({xx, (d.first + d.second) * 0.5f, (zf + door_top) * 0.5f}, {0.012f, (d.second - d.first) * 0.5f, (door_top - zf) * 0.5f}, st, kMatMetal);
      g.box({xx - sx * 0.014f, (d.first + d.second) * 0.5f, (s.zWin0 + s.zWin1) * 0.5f}, {0.004f, (d.second - d.first) * 0.5f - 0.12f, (s.zWin1 - s.zWin0) * 0.5f - 0.05f},
            Color{30, 34, 38, 255}, kGlass);
    }
}

void interior(Geo& g, const Spec& s, float y0, float y1, const std::vector<std::pair<float, float>>& doors,
              const std::vector<std::pair<float, float>>& windows) {
  const float zf = s.zFloor + kFloorAbove;
  g.quad({-s.W + 0.05f, y0, zf}, {s.W - 0.05f, y0, zf}, {s.W - 0.05f, y1, zf}, {-s.W + 0.05f, y1, zf}, s.shinkansen ? Color{90, 84, 80, 255} : Color{120, 116, 110, 255},
         kShell);
  // ceiling: flat centre with sloping sides, light strips
  const float zc = s.zSide - 0.1f, zc2 = s.zSide + 0.12f;
  g.quad({-0.7f, y1, zc2}, {0.7f, y1, zc2}, {0.7f, y0, zc2}, {-0.7f, y0, zc2}, Color{232, 232, 228, 255}, kShell);
  for (float sx : {-1.0f, 1.0f}) {
    if (sx > 0) g.quad({0.7f, y1, zc2}, {s.W - 0.05f, y1, zc}, {s.W - 0.05f, y0, zc}, {0.7f, y0, zc2}, Color{226, 226, 222, 255}, kShell);
    else g.quad({-(s.W - 0.05f), y1, zc}, {-0.7f, y1, zc2}, {-0.7f, y0, zc2}, {-(s.W - 0.05f), y0, zc}, Color{226, 226, 222, 255}, kShell);
  }
  for (float x : {-0.55f, 0.55f})  // light strips (lit diffusers)
    g.quad({x - 0.07f, y1 - 0.5f, zc2 - 0.015f}, {x + 0.07f, y1 - 0.5f, zc2 - 0.015f}, {x + 0.07f, y0 + 0.5f, zc2 - 0.015f}, {x - 0.07f, y0 + 0.5f, zc2 - 0.015f},
           Color{255, 255, 250, 255}, kShell);
  innerWalls(g, s, y0, y1, windows, doors, s.shinkansen ? Color{234, 230, 220, 255} : Color{226, 226, 222, 255});
  if (s.shinkansen) {
    // rows of 2+3 reclining seats facing forward: cushion, back, headrest cover, armrests, tray table
    const Color seat{38, 58, 118, 255}, cover{236, 236, 232, 255}, arm{70, 72, 78, 255};
    for (float y = y0 + 1.5f; y < y1 - 1.0f; y += kShinkansenSeatPitch) {
      for (float x : {-1.35f, -0.9f, 0.35f, 0.82f, 1.29f}) {
        g.box({x, y, zf + 0.42f}, {0.21f, 0.24f, 0.06f}, seat, kShell);
        g.box({x, y, zf + 0.2f}, {0.2f, 0.18f, 0.17f}, Color{50, 52, 58, 255}, kShell);
        g.box({x, y - 0.27f, zf + 0.85f}, {0.21f, 0.06f, 0.42f}, seat, kShell);
        g.box({x, y - 0.24f, zf + 1.18f}, {0.18f, 0.035f, 0.09f}, cover, kShell);
        g.box({x, y - 0.335f, zf + 0.78f}, {0.17f, 0.006f, 0.13f}, Color{150, 150, 152, 255}, kShell);  // folded tray table
      }
      for (float x : {-1.58f, -1.125f, -0.67f, 0.12f, 0.585f, 1.055f, 1.52f})
        g.box({x, y - 0.05f, zf + 0.6f}, {0.025f, 0.2f, 0.03f}, arm, kShell);
    }
    // overhead luggage racks
    for (float sx : {-1.0f, 1.0f}) {
      g.box({sx * (s.W - 0.32f), (y0 + y1) * 0.5f, zf + 1.78f}, {0.28f, (y1 - y0) * 0.5f - 0.6f, 0.02f}, Color{200, 202, 206, 255}, kMatMetal);
      g.box({sx * (s.W - 0.6f), (y0 + y1) * 0.5f, zf + 1.74f}, {0.015f, (y1 - y0) * 0.5f - 0.6f, 0.04f}, Color{190, 192, 196, 255}, kMatMetal);
    }
    // end partition with a glass sliding door and an information display above it
    const float ye = y1 - 0.3f;
    g.quad({-s.W + 0.05f, ye, zf}, {-0.45f, ye, zf}, {-0.45f, ye, zc}, {-s.W + 0.05f, ye, zc}, Color{220, 214, 204, 255}, kShell);
    g.quad({0.45f, ye, zf}, {s.W - 0.05f, ye, zf}, {s.W - 0.05f, ye, zc}, {0.45f, ye, zc}, Color{220, 214, 204, 255}, kShell);
    g.quad({-0.45f, ye, zf + 2.0f}, {0.45f, ye, zf + 2.0f}, {0.45f, ye, zc}, {-0.45f, ye, zc}, Color{220, 214, 204, 255}, kShell);
    g.box({0, ye - 0.02f, zf + 1.0f}, {0.42f, 0.01f, 1.0f}, Color{40, 46, 52, 255}, kGlass);
    g.box({0, ye - 0.03f, zf + 2.12f}, {0.4f, 0.01f, 0.07f}, Color{255, 160, 40, 255}, kGlass);
  } else {
    // long bench seats between the doors, luggage racks, straps, grab poles, displays, hanging ads
    std::vector<float> cuts = {y0 + 0.4f};
    for (const auto& d : doors) cuts.insert(cuts.end(), {d.first - 0.15f, d.second + 0.15f});
    cuts.push_back(y1 - 0.4f);
    const Color bench{60, 110, 90, 255}, rack{190, 192, 196, 255};
    for (size_t k = 0; k + 1 < cuts.size(); k += 2) {
      const float a = cuts[k], b = cuts[k + 1];
      if (b - a < 0.6f) continue;
      for (float sx : {-1.0f, 1.0f}) {
        g.box({sx * (s.W - 0.32f), (a + b) / 2, zf + 0.42f}, {0.26f, (b - a) / 2, 0.06f}, bench, kShell);
        g.box({sx * (s.W - 0.36f), (a + b) / 2, zf + 0.2f}, {0.2f, (b - a) / 2, 0.17f}, Color{70, 72, 76, 255}, kShell);
        g.box({sx * (s.W - 0.1f), (a + b) / 2, zf + 0.72f}, {0.05f, (b - a) / 2, 0.26f}, bench, kShell);  // backrest
        g.box({sx * (s.W - 0.3f), (a + b) / 2, zf + 1.86f}, {0.24f, (b - a) / 2, 0.012f}, rack, kMatMetal);  // luggage rack
        g.box({sx * (s.W - 0.08f), a + 0.03f, zf + 0.9f}, {0.3f, 0.02f, 0.5f}, Color{200, 202, 206, 255}, kMatMetal);   // end screens
        g.box({sx * (s.W - 0.08f), b - 0.03f, zf + 0.9f}, {0.3f, 0.02f, 0.5f}, Color{200, 202, 206, 255}, kMatMetal);
        // straps on a rail above the seat front
        g.box({sx * 0.78f, (a + b) / 2, zf + 1.98f}, {0.012f, (b - a) / 2, 0.012f}, rack, kMatMetal);
        for (float y = a + 0.2f; y < b - 0.1f; y += 0.32f) {
          g.box({sx * 0.78f, y, zf + 1.86f}, {0.012f, 0.012f, 0.11f}, Color{240, 240, 236, 255}, kShell);
          g.box({sx * 0.78f, y, zf + 1.72f}, {0.012f, 0.05f, 0.05f}, Color{230, 230, 60, 255}, kShell);
        }
      }
    }
    for (const auto& d : doors) {
      for (float sx : {-1.0f, 1.0f}) {
        for (float yy : {d.first - 0.3f, d.second + 0.3f})  // grab poles by the doors
          g.box({sx * (s.W - 0.6f), yy, (zf + s.zSide) * 0.5f}, {0.018f, 0.018f, (s.zSide - zf) * 0.5f}, Color{205, 205, 208, 255}, kMatMetal);
        g.box({sx * (s.W - 0.07f), (d.first + d.second) * 0.5f, s.zWin1 + 0.24f}, {0.02f, 0.32f, 0.09f}, Color{20, 22, 26, 255}, kShell);  // display
        g.box({sx * (s.W - 0.092f), (d.first + d.second) * 0.5f, s.zWin1 + 0.24f}, {0.002f, 0.28f, 0.07f}, Color{255, 170, 60, 255}, kGlass);
      }
    }
    // hanging advertisement sheets along the middle (generic colours, no text)
    int k = 0;
    for (float y = y0 + 2.4f; y < y1 - 2.0f; y += 1.7f, ++k) {
      const Color pc[4] = {{236, 210, 170, 255}, {170, 206, 236, 255}, {236, 180, 190, 255}, {196, 230, 180, 255}};
      g.box({0, y, zf + 1.83f}, {0.26f, 0.004f, 0.18f}, pc[k % 4], kShell);
    }
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
  if (cab) {
    // front face with a real windscreen opening (the driver looks through it), black mask, band, lamps
    const Color fc{40, 42, 46, 255};
    const float wz0 = 1.9f, wz1 = 2.85f, wx = 1.3f;
    auto F = [&](float xa, float xb, float za, float zb) { g.quad({xa, y1, za}, {xb, y1, za}, {xb, y1, zb}, {xa, y1, zb}, fc, kShell); };
    F(-s.W, s.W, s.zFloor, wz0);
    F(-s.W, s.W, wz1, s.zSide);
    F(-s.W, -wx, wz0, wz1);
    F(wx, s.W, wz0, wz1);
    const Vector3 prof[] = {{s.W, y1, s.zSide}, {s.shoulderX, y1, s.zShoulder}, {0, y1, s.zRoof}, {-s.shoulderX, y1, s.zShoulder}, {-s.W, y1, s.zSide}};
    for (int i = 0; i + 1 < 5; ++i) g.quad(prof[i + 1], prof[i], {0, y1, s.zSide}, {0, y1, s.zSide}, fc, kShell);
    glass.quad({-wx, y1 + 0.02f, wz0}, {wx, y1 + 0.02f, wz0}, {wx, y1 + 0.02f, wz1}, {-wx, y1 + 0.02f, wz1}, Color{255, 255, 255, 255}, kGlass);
    g.box({0, y1 + 0.03f, 1.43f}, {1.44f, 0.02f, 0.07f}, s.band, kShell);
    for (float x : {-1.0f, 1.0f}) g.box({x, y1 + 0.04f, 1.2f}, {0.18f, 0.02f, 0.08f}, Color{255, 250, 235, 255}, kMatSignalLamp);
    // wipers at the base of the windscreen, destination display above
    g.box({-0.6f, y1 + 0.04f, wz0 + 0.3f}, {0.02f, 0.01f, 0.32f}, Color{20, 20, 20, 255}, kDark);
    g.box({0.6f, y1 + 0.04f, wz0 + 0.3f}, {0.02f, 0.01f, 0.32f}, Color{20, 20, 20, 255}, kDark);
    g.box({0, y1 + 0.03f, 3.08f}, {0.7f, 0.02f, 0.1f}, Color{255, 170, 60, 255}, kGlass);
  } else {
    endWall(g, s, y1, 1, s.body);
  }
  underframe(g, s, y0, y1);
  // pantograph frame on the roof
  g.box({0, 2.0f, s.zRoof + 0.15f}, {0.9f, 0.6f, 0.03f}, Color{80, 80, 80, 255}, kMatMetal);
  const float cab_back = y1 - 1.6f;
  interior(in, s, y0, cab ? cab_back : y1, doors, windows);
  if (cab) {  // driver's cab: partition, desk with the master controller and gauges, seat
    const float zf = s.zFloor + kFloorAbove;
    const Color wall{210, 208, 202, 255}, desk{52, 54, 58, 255};
    in.quad({-s.W + 0.05f, cab_back, zf}, {s.W - 0.05f, cab_back, zf}, {s.W - 0.05f, cab_back, s.zSide}, {-s.W + 0.05f, cab_back, s.zSide}, wall, kShell);
    in.quad({-s.W + 0.05f, cab_back, zf}, {s.W - 0.05f, cab_back, zf}, {s.W - 0.05f, y1 - 0.05f, zf}, {-s.W + 0.05f, y1 - 0.05f, zf}, Color{90, 90, 92, 255}, kShell);
    in.quad({-s.W + 0.05f, y1 - 0.05f, s.zSide - 0.05f}, {s.W - 0.05f, y1 - 0.05f, s.zSide - 0.05f}, {s.W - 0.05f, cab_back, s.zSide - 0.05f},
            {-s.W + 0.05f, cab_back, s.zSide - 0.05f}, Color{200, 200, 196, 255}, kShell);
    for (float sx : {-1.0f, 1.0f}) {  // cab side walls with the side window
      const float x = sx * (s.W - 0.05f);
      if (sx > 0) in.quad({x, y1 - 0.05f, zf}, {x, cab_back, zf}, {x, cab_back, s.zWin0}, {x, y1 - 0.05f, s.zWin0}, wall, kShell);
      else in.quad({x, cab_back, zf}, {x, y1 - 0.05f, zf}, {x, y1 - 0.05f, s.zWin0}, {x, cab_back, s.zWin0}, wall, kShell);
    }
    in.box({-0.48f, y1 - 0.42f, zf + 0.62f}, {0.62f, 0.3f, 0.34f}, desk, kShell);                                             // desk
    in.box({-0.48f, y1 - 0.5f, zf + 1.02f}, {0.6f, 0.2f, 0.06f}, Color{36, 36, 40, 255}, kShell);                              // desk top
    in.box({-0.62f, y1 - 0.55f, zf + 1.1f}, {0.13f, 0.02f, 0.07f}, Color{10, 10, 12, 255}, kShell);                            // speedometer face
    in.box({-0.62f, y1 - 0.565f, zf + 1.1f}, {0.1f, 0.004f, 0.05f}, Color{255, 170, 60, 255}, kGlass);                         // lit gauge
    in.box({-0.18f, y1 - 0.62f, zf + 1.12f}, {0.03f, 0.08f, 0.05f}, Color{20, 20, 22, 255}, kShell);                           // master controller
    in.box({-0.18f, y1 - 0.7f, zf + 1.18f}, {0.08f, 0.02f, 0.02f}, Color{200, 40, 40, 255}, kShell);                           // its handle
    in.box({-0.55f, cab_back + 0.55f, zf + 0.42f}, {0.24f, 0.22f, 0.06f}, Color{40, 60, 90, 255}, kShell);                     // seat
    in.box({-0.55f, cab_back + 0.3f, zf + 0.75f}, {0.24f, 0.05f, 0.3f}, Color{40, 60, 90, 255}, kShell);
    in.box({0.5f, y1 - 0.35f, zf + 0.45f}, {0.35f, 0.25f, 0.45f}, Color{120, 122, 126, 255}, kShell);                         // cabinet
  }
  TrainCarModel m;
  m.length = s.L;
  m.shell = g.upload();
  m.glass = glass.upload();
  m.interior = in.upload();
  return m;
}

TrainCarModel makeShinkansen(bool nose, bool panto = false) {
  Spec s{25.0f, 1.69f, 1.1f, 2.05f, 2.55f, 3.1f, 3.48f, 3.65f, 1.3f, {242, 242, 240, 255}, {26, 64, 160, 255}, {242, 242, 240, 255},
         1.38f, 1.55f, true};
  Geo g, glass, in;
  const float y0 = -s.L / 2;
  const float y1 = nose ? s.L / 2 - 11.5f : s.L / 2;
  std::vector<std::pair<float, float>> windows, doors = {{y0 + 0.6f, y0 + 1.6f}};
  for (float y = y0 + 2.4f; y < y1 - 0.9f; y += 1.04f) windows.push_back({y, y + 0.62f});
  for (int sx = -1; sx <= 1; sx += 2) sideWall(g, glass, s, static_cast<float>(sx), y0, y1, windows, doors);
  roof(g, s, y0, y1);
  endWall(g, s, y0, -1, s.body);
  // thin second stripe under the band, full-length side skirts over the bogies (typical of high-speed cars)
  for (float sx : {-1.0f, 1.0f}) {
    const float x = sx * (s.W + 0.004f);
    auto Q = [&](float ya, float yb, float za, float zb, Color c) {
      if (sx > 0) g.quad({x, ya, za}, {x, yb, za}, {x, yb, zb}, {x, ya, zb}, c, kShell);
      else g.quad({x, yb, za}, {x, ya, za}, {x, ya, zb}, {x, yb, zb}, c, kShell);
    };
    Q(y0, y1, 1.24f, 1.28f, s.band);
    const float xs = sx * (s.W - 0.06f);
    const Color skirt{214, 216, 218, 255};
    if (sx > 0) g.quad({xs, y0 + 0.2f, 0.55f}, {xs, y1 - 0.2f, 0.55f}, {xs, y1 - 0.2f, s.zFloor}, {xs, y0 + 0.2f, s.zFloor}, skirt, kShell);
    else g.quad({xs, y1 - 0.2f, 0.55f}, {xs, y0 + 0.2f, 0.55f}, {xs, y0 + 0.2f, s.zFloor}, {xs, y1 - 0.2f, s.zFloor}, skirt, kShell);
  }
  // gangway bellows at the car ends
  const Color bel{30, 30, 32, 255};
  g.box({0, y0 - 0.12f, (s.zFloor + s.zSide) * 0.5f}, {s.W - 0.28f, 0.14f, (s.zSide - s.zFloor) * 0.5f - 0.1f}, bel, kDark);
  if (!nose) g.box({0, y1 + 0.12f, (s.zFloor + s.zSide) * 0.5f}, {s.W - 0.28f, 0.14f, (s.zSide - s.zFloor) * 0.5f - 0.1f}, bel, kDark);
  if (panto) {  // single-arm pantograph with its noise shield
    const float py = y0 + 5.0f, pz = s.zRoof;
    const Color pc{90, 92, 96, 255};
    g.box({0, py, pz + 0.06f}, {0.55f, 1.1f, 0.06f}, Color{120, 122, 126, 255}, kMatMetal);
    for (float sx : {-1.0f, 1.0f}) g.box({sx * 0.95f, py, pz + 0.18f}, {0.03f, 1.6f, 0.2f}, Color{228, 228, 226, 255}, kShell);  // shield
    Geo& h = g;
    const Vector3 a{0, py - 0.6f, pz + 0.12f}, b{0, py + 0.4f, pz + 0.9f}, c{0, py - 0.2f, pz + 1.6f};
    auto bar = [&](Vector3 p, Vector3 q) {
      const Vector3 m{(p.x + q.x) / 2, (p.y + q.y) / 2, (p.z + q.z) / 2};
      h.box(m, {0.035f, std::fabs(q.y - p.y) / 2 + 0.03f, std::fabs(q.z - p.z) / 2 + 0.03f}, pc, kMatMetal);
    };
    bar(a, b);
    bar(b, c);
    g.box({0, c.y, c.z + 0.03f}, {0.75f, 0.06f, 0.025f}, Color{60, 60, 62, 255}, kMatMetal);  // pan head
  }
  if (!nose) {
    endWall(g, s, y1, 1, s.body);
  } else {
    // long aerodynamic nose (generic): the section narrows and drops, a canopy hump over the cab,
    // the blue band sweeps down towards the tip; cab windscreen, headlights low on the flanks
    const int nr = 30, ns = 12;
    const float Ln = s.L / 2 - y1;
    auto smooth = [](float e0, float e1, float x) {
      const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
      return t * t * (3 - 2 * t);
    };
    std::vector<std::vector<Vector3>> rings;
    std::vector<float> ts;
    for (int i = 0; i <= nr; ++i) {
      const float t = static_cast<float>(i) / nr;
      const float y = y1 + t * Ln;
      const float w = std::max(0.08f, s.W * (1.0f - 0.86f * std::pow(t, 2.1f)));
      const float hump = 0.22f * std::exp(-std::pow((t - 0.40f) / 0.11f, 2.0f));
      const float zt = s.zRoof - 2.25f * std::pow(smooth(0.03f, 1.0f, t), 1.15f) + hump * (1.0f - t);
      const float zb = s.zFloor - 0.2f + 0.5f * std::pow(t, 2.4f);
      const float zm = std::min(zt - 0.12f, std::max(zb + 0.08f, s.zSide - 1.25f * std::pow(t, 1.2f) - 0.35f * t));
      std::vector<Vector3> r;
      // lower flank (vertical), then a superellipse cap from zm up to zt
      r.push_back({w, y, zb});
      for (int k = 0; k <= ns; ++k) {
        const float th = PI * k / ns;
        const float c = std::cos(th), sn = std::sin(th);
        const float ex = 2.0f / 3.2f;
        const float x = w * (c >= 0 ? 1.0f : -1.0f) * std::pow(std::fabs(c), ex);
        const float z = zm + (zt - zm) * std::pow(sn, ex);
        r.push_back({x, y, z});
      }
      r.push_back({-w, y, zb});
      rings.push_back(r);
      ts.push_back(t);
    }
    const size_t np = rings[0].size();
    for (int i = 0; i < nr; ++i) {
      const float t = (ts[static_cast<size_t>(i)] + ts[static_cast<size_t>(i) + 1]) * 0.5f;
      const float band_c = 1.465f - 0.25f * t, band_h = 0.085f * (1.0f - 0.5f * t);
      for (size_t k = 0; k + 1 < np; ++k) {
        const Vector3 a = rings[static_cast<size_t>(i)][k], b = rings[static_cast<size_t>(i)][k + 1];
        const Vector3 c = rings[static_cast<size_t>(i) + 1][k + 1], d = rings[static_cast<size_t>(i) + 1][k];
        const float zc = (a.z + b.z + c.z + d.z) * 0.25f, xc = (a.x + b.x + c.x + d.x) * 0.25f;
        const bool cab = t > 0.28f && t < 0.5f && zc > rings[static_cast<size_t>(i)][1].z + 0.05f && std::fabs(xc) < s.W * 0.62f;
        const bool band = std::fabs(zc - band_c) < band_h;
        Color col = cab ? Color{18, 20, 24, 255} : band ? s.band : s.body;
        g.quad(a, d, c, b, col, cab ? kGlass : kShell);
      }
      // underside
      const Vector3 a = rings[static_cast<size_t>(i)].front(), b = rings[static_cast<size_t>(i)].back();
      const Vector3 c = rings[static_cast<size_t>(i) + 1].back(), d = rings[static_cast<size_t>(i) + 1].front();
      g.quad(b, c, d, a, Color{60, 62, 66, 255}, kDark);
    }
    // tip cap
    const auto& last = rings.back();
    for (size_t k = 1; k + 1 < last.size(); ++k) g.quad(last[0], last[k], last[k + 1], last[k + 1], s.body, kShell);
    // headlights (paired lamps low on the flanks of the nose)
    const float tl = 0.8f, yl = y1 + tl * Ln;
    const float wl = s.W * (1.0f - 0.86f * std::pow(tl, 2.1f));
    for (float sx : {-1.0f, 1.0f})
      g.box({sx * wl * 0.72f, yl, 1.42f}, {0.12f, 0.18f, 0.035f}, Color{255, 250, 235, 255}, kMatSignalLamp);
    // nose-end skirt fairing
    g.box({0, y1 + 1.5f, 0.75f}, {s.W - 0.1f, 1.5f, 0.2f}, Color{214, 216, 218, 255}, kShell);
  }
  underframe(g, s, y0, nose ? y1 + 3.0f : y1);
  interior(in, s, y0, y1, doors, windows);
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
  m_[static_cast<int>(TrainCar::ShinkansenPanto)] = makeShinkansen(false, true);
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
