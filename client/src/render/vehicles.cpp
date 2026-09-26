#include "render/vehicles.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

// Model space while building: x right, y forward, z up (converted to raylib space on upload).
struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short v() const { return static_cast<unsigned short>(pos.size() / 3); }
  void vert(Vector3 p, Vector3 n, Color c, int mat) {
    pos.insert(pos.end(), {p.x, p.z, -p.y});  // -> raylib (x, y up, z = -forward)
    nrm.insert(nrm.end(), {n.x, n.z, -n.y});
    uv.insert(uv.end(), {0.0f, 0.0f});
    col.insert(col.end(), {c.r, c.g, c.b, c.a});
    t2.insert(t2.end(), {static_cast<float>(mat), 0.0f});
  }
  void tri(unsigned short a, unsigned short b, unsigned short c) { idx.insert(idx.end(), {a, b, c}); }
  // Flat-shaded quad (a b c d counter-clockwise seen from outside).
  void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col, int mat) {
    const Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a)));
    const unsigned short k = v();
    vert(a, n, col, mat);
    vert(b, n, col, mat);
    vert(c, n, col, mat);
    vert(d, n, col, mat);
    tri(k, static_cast<unsigned short>(k + 1), static_cast<unsigned short>(k + 2));
    tri(k, static_cast<unsigned short>(k + 2), static_cast<unsigned short>(k + 3));
  }
  void box(Vector3 c, Vector3 h, Color col, int mat) {
    const Vector3 p[8] = {{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z - h.z},
                          {c.x - h.x, c.y + h.y, c.z - h.z}, {c.x - h.x, c.y - h.y, c.z + h.z}, {c.x + h.x, c.y - h.y, c.z + h.z},
                          {c.x + h.x, c.y + h.y, c.z + h.z}, {c.x - h.x, c.y + h.y, c.z + h.z}};
    quad(p[4], p[5], p[6], p[7], col, mat);  // top
    quad(p[3], p[2], p[1], p[0], col, mat);  // bottom
    quad(p[0], p[1], p[5], p[4], col, mat);  // rear (-y)
    quad(p[2], p[3], p[7], p[6], col, mat);  // front (+y)
    quad(p[1], p[2], p[6], p[5], col, mat);  // right
    quad(p[3], p[0], p[4], p[7], col, mat);  // left
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

const Color kPaint{255, 255, 255, 255};  // tinted per vehicle via colDiffuse
const Color kTrim{20, 20, 22, 255};

// Loft: cross-sections along y (forward). Each section: y, z_bottom, z_top, half width bottom,
// half width top. Side faces take `side_mat`, the top face `top_mat`; both ends are capped.
struct Sec {
  float y, zb, zt, wb, wt;
};
void loft(Geo& g, const std::vector<Sec>& s, int side_mat, int top_mat, int end_mat_front, int end_mat_rear, bool caps = true) {
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    const Sec& a = s[i];
    const Sec& b = s[i + 1];
    // right side
    g.quad({a.wb, a.y, a.zb}, {b.wb, b.y, b.zb}, {b.wt, b.y, b.zt}, {a.wt, a.y, a.zt}, kPaint, side_mat);
    // left side
    g.quad({-b.wb, b.y, b.zb}, {-a.wb, a.y, a.zb}, {-a.wt, a.y, a.zt}, {-b.wt, b.y, b.zt}, kPaint, side_mat);
    // top
    g.quad({a.wt, a.y, a.zt}, {b.wt, b.y, b.zt}, {-b.wt, b.y, b.zt}, {-a.wt, a.y, a.zt}, kPaint, top_mat);
    // underside
    g.quad({-a.wb, a.y, a.zb}, {-b.wb, b.y, b.zb}, {b.wb, b.y, b.zb}, {a.wb, a.y, a.zb}, kTrim, kMatTyre);
  }
  if (!caps) return;
  const Sec& f = s.back();
  g.quad({f.wb, f.y, f.zb}, {-f.wb, f.y, f.zb}, {-f.wt, f.y, f.zt}, {f.wt, f.y, f.zt}, kPaint, end_mat_front);
  const Sec& r = s.front();
  g.quad({-r.wb, r.y, r.zb}, {r.wb, r.y, r.zb}, {r.wt, r.y, r.zt}, {-r.wt, r.y, r.zt}, kPaint, end_mat_rear);
}

// Greenhouse: glass sides, painted roof, sloped windscreen / rear window (glass).
void cabin(Geo& g, float y0, float y1, float zbelt, float zroof, float rear_slope, float front_slope, float wb, float wt) {
  const float yr = y0 + rear_slope, yf = y1 - front_slope;
  // sides (trapezoids in side view)
  g.quad({wb, y0, zbelt}, {wb, y1, zbelt}, {wt, yf, zroof}, {wt, yr, zroof}, kPaint, kMatGlass);
  g.quad({-wb, y1, zbelt}, {-wb, y0, zbelt}, {-wt, yr, zroof}, {-wt, yf, zroof}, kPaint, kMatGlass);
  // windscreen, rear window
  g.quad({wb, y1, zbelt}, {-wb, y1, zbelt}, {-wt, yf, zroof}, {wt, yf, zroof}, kPaint, kMatGlass);
  g.quad({-wb, y0, zbelt}, {wb, y0, zbelt}, {wt, yr, zroof}, {-wt, yr, zroof}, kPaint, kMatGlass);
  // roof (paint), pillars implied by the paint roof edge
  g.quad({wt, yr, zroof}, {wt, yf, zroof}, {-wt, yf, zroof}, {-wt, yr, zroof}, kPaint, kMatCarPaint);
  // B-pillar strips (paint) on both sides for a less "fishbowl" look
  const float ym = (yr + yf) * 0.5f;
  for (float sgn : {1.0f, -1.0f}) {
    const float x0 = sgn * (wb + 0.004f), x1 = sgn * (wt + 0.004f);
    g.quad({x0, ym - 0.05f, zbelt}, {x0, ym + 0.05f, zbelt}, {x1, ym + 0.05f, zroof}, {x1, ym - 0.05f, zroof}, kTrim, kMatTyre);
  }
}

void plates(Geo& g, float yf, float yr, float z) {
  const Color white{235, 235, 228, 255};
  g.quad({-0.165f, yf + 0.01f, z - 0.08f}, {0.165f, yf + 0.01f, z - 0.08f}, {0.165f, yf + 0.01f, z + 0.08f}, {-0.165f, yf + 0.01f, z + 0.08f},
         white, kMatUntinted);
  g.quad({0.165f, yr - 0.01f, z - 0.08f}, {-0.165f, yr - 0.01f, z - 0.08f}, {-0.165f, yr - 0.01f, z + 0.08f}, {0.165f, yr - 0.01f, z + 0.08f},
         white, kMatUntinted);
}

void mirrors(Geo& g, float y, float z, float w) {
  g.box({w + 0.08f, y, z}, {0.07f, 0.04f, 0.05f}, kTrim, kMatTyre);
  g.box({-w - 0.08f, y, z}, {0.07f, 0.04f, 0.05f}, kTrim, kMatTyre);
}

void lamps(Geo& head, Geo& tail, float yf, float yr, float z, float w, float lw) {
  const Color c{255, 255, 255, 255};
  head.box({w - lw * 0.5f, yf - 0.02f, z}, {lw * 0.5f, 0.04f, 0.055f}, c, kMatSignalLamp);
  head.box({-w + lw * 0.5f, yf - 0.02f, z}, {lw * 0.5f, 0.04f, 0.055f}, c, kMatSignalLamp);
  tail.box({w - lw * 0.4f, yr + 0.02f, z + 0.05f}, {lw * 0.4f, 0.04f, 0.07f}, c, kMatSignalLamp);
  tail.box({-w + lw * 0.4f, yr + 0.02f, z + 0.05f}, {lw * 0.4f, 0.04f, 0.07f}, c, kMatSignalLamp);
}

VehicleModel makeCar(float L, float W, float zroof, float zbelt, float hood_y, float trunk_y, float rear_slope, float front_slope,
                     float wheel_r, bool boxy, bool taxi_sign = false) {
  Geo body, head, tail;
  const float hw = W * 0.5f, yf = L * 0.5f, yr = -L * 0.5f;
  const float zb = 0.18f;
  // Lower body with a slightly lower hood/boot line and rounded ends.
  const float znose = zbelt - (boxy ? 0.05f : 0.14f), ztail = zbelt - (boxy ? 0.02f : 0.06f);
  std::vector<Sec> s = {{yr, zb + 0.12f, ztail - 0.05f, hw - 0.10f, hw - 0.14f}, {yr + 0.12f, zb + 0.02f, ztail, hw - 0.03f, hw - 0.07f},
                        {yr + 0.5f, zb, zbelt, hw, hw - 0.05f},                    {yf - 0.6f, zb, zbelt - 0.02f, hw, hw - 0.05f},
                        {yf - 0.14f, zb + 0.03f, znose, hw - 0.04f, hw - 0.09f},  {yf, zb + 0.14f, znose - 0.06f, hw - 0.12f, hw - 0.18f}};
  loft(body, s, kMatCarPaint, kMatCarPaint, kMatCarPaint, kMatCarPaint);
  cabin(body, trunk_y, hood_y, zbelt, zroof, rear_slope, front_slope, hw - 0.06f, hw - (boxy ? 0.12f : 0.22f));
  // bumpers / sills (black trim)
  body.box({0, yf - 0.05f, zb + 0.12f}, {hw - 0.1f, 0.06f, 0.08f}, kTrim, kMatTyre);
  body.box({0, yr + 0.05f, zb + 0.12f}, {hw - 0.1f, 0.06f, 0.08f}, kTrim, kMatTyre);
  plates(body, yf - 0.06f, yr + 0.06f, zb + 0.24f);
  mirrors(body, hood_y - 0.1f, zbelt + 0.08f, hw - 0.06f);
  lamps(head, tail, yf - 0.1f, yr + 0.1f, zbelt - 0.12f, hw - 0.08f, 0.32f);
  if (taxi_sign)  // roof sign ("andon")
    body.box({0, (trunk_y + hood_y) * 0.5f, zroof + 0.09f}, {0.22f, 0.07f, 0.09f}, Color{236, 226, 170, 255}, kMatUntinted);
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = wheel_r;
  const float wx = hw - 0.12f, wyf = yf - 0.85f, wyr = yr + 0.8f;
  const float wp[4][2] = {{wx, wyf}, {-wx, wyf}, {wx, wyr}, {-wx, wyr}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  return m;
}

VehicleModel makeBus() {
  Geo body, head, tail;
  const float L = 10.5f, W = 2.49f, hw = W * 0.5f, yf = L * 0.5f, yr = -L * 0.5f;
  const float zb = 0.3f, zwin = 1.15f, zwt = 2.55f, zr = 3.05f;
  // lower body, window band, roof
  std::vector<Sec> lower = {{yr, zb, zwin, hw, hw}, {yf, zb, zwin, hw, hw}};
  loft(body, lower, kMatCarPaint, kMatCarPaint, kMatCarPaint, kMatCarPaint);
  std::vector<Sec> band = {{yr, zwin, zwt, hw, hw - 0.02f}, {yf - 0.05f, zwin, zwt, hw, hw - 0.02f}};
  loft(body, band, kMatGlass, kMatGlass, kMatGlass, kMatGlass, true);
  std::vector<Sec> roof = {{yr, zwt, zr, hw - 0.02f, hw - 0.12f}, {yf - 0.05f, zwt, zr, hw - 0.02f, hw - 0.12f}};
  loft(body, roof, kMatCarPaint, kMatCarPaint, kMatCarPaint, kMatCarPaint);
  // livery stripe (Tokyo city buses carry a coloured band) and window pillars
  const Color stripe{40, 130, 90, 255};
  body.quad({hw + 0.005f, yr, zwin - 0.25f}, {hw + 0.005f, yf, zwin - 0.25f}, {hw + 0.005f, yf, zwin - 0.05f}, {hw + 0.005f, yr, zwin - 0.05f}, stripe,
            kMatUntinted);
  body.quad({-hw - 0.005f, yf, zwin - 0.25f}, {-hw - 0.005f, yr, zwin - 0.25f}, {-hw - 0.005f, yr, zwin - 0.05f}, {-hw - 0.005f, yf, zwin - 0.05f},
            stripe, kMatUntinted);
  for (float y = yr + 1.2f; y < yf - 1.0f; y += 1.45f) {
    body.box({hw + 0.01f, y, (zwin + zwt) * 0.5f}, {0.012f, 0.05f, (zwt - zwin) * 0.5f}, kTrim, kMatTyre);
    body.box({-hw - 0.01f, y, (zwin + zwt) * 0.5f}, {0.012f, 0.05f, (zwt - zwin) * 0.5f}, kTrim, kMatTyre);
  }
  body.box({0, yr + 0.3f, zr + 0.12f}, {0.7f, 1.0f, 0.12f}, Color{200, 200, 196, 255}, kMatUntinted);  // roof AC unit
  plates(body, yf, yr, zb + 0.35f);
  lamps(head, tail, yf, yr, zb + 0.45f, hw - 0.1f, 0.3f);
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = 0.48f;
  const float wx = hw - 0.25f;
  const float wp[4][2] = {{wx, yf - 2.2f}, {-wx, yf - 2.2f}, {wx, yr + 2.6f}, {-wx, yr + 2.6f}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  return m;
}

VehicleModel makeTruck() {
  Geo body, head, tail;
  const float L = 6.2f, W = 1.95f, hw = W * 0.5f, yf = L * 0.5f, yr = -L * 0.5f;
  // cab-over cab (2 t class): short, tall
  const float cab_r = yf - 1.75f;
  std::vector<Sec> cab = {{cab_r, 0.45f, 1.35f, hw, hw - 0.04f}, {yf - 0.05f, 0.45f, 1.30f, hw, hw - 0.06f}, {yf, 0.55f, 1.2f, hw - 0.05f, hw - 0.1f}};
  loft(body, cab, kMatCarPaint, kMatCarPaint, kMatCarPaint, kMatCarPaint);
  cabin(body, cab_r, yf, 1.3f, 2.15f, 0.02f, 0.25f, hw - 0.02f, hw - 0.08f);
  // aluminium cargo box
  const Color alu{205, 207, 210, 255};
  body.box({0, (yr + cab_r - 0.1f) * 0.5f, 1.75f}, {hw + 0.05f, (cab_r - 0.1f - yr) * 0.5f, 1.2f}, alu, kMatUntinted);
  body.box({0, (yr + cab_r) * 0.5f, 0.55f}, {hw - 0.15f, (cab_r - yr) * 0.5f, 0.12f}, kTrim, kMatTyre);  // chassis
  plates(body, yf, yr, 0.6f);
  lamps(head, tail, yf, yr, 0.75f, hw - 0.1f, 0.26f);
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = 0.38f;
  const float wx = hw - 0.2f;
  const float wp[4][2] = {{wx, yf - 1.0f}, {-wx, yf - 1.0f}, {wx, yr + 1.4f}, {-wx, yr + 1.4f}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  return m;
}

}  // namespace

void VehicleModels::build() {
  if (ready_) return;
  // (length, width, roof, belt, cabin front y, cabin rear y, rear slope, front slope, wheel r, boxy)
  models_[static_cast<int>(VehicleType::Sedan)] = makeCar(4.7f, 1.8f, 1.44f, 0.92f, 0.95f, -1.35f, 0.55f, 0.75f, 0.31f, false);
  models_[static_cast<int>(VehicleType::Taxi)] = makeCar(4.4f, 1.7f, 1.74f, 0.98f, 0.85f, -2.05f, 0.12f, 0.55f, 0.30f, true, true);
  models_[static_cast<int>(VehicleType::Kei)] = makeCar(3.4f, 1.48f, 1.78f, 0.95f, 0.95f, -1.62f, 0.06f, 0.7f, 0.27f, true);
  models_[static_cast<int>(VehicleType::Minivan)] = makeCar(4.9f, 1.85f, 1.9f, 1.02f, 1.25f, -2.35f, 0.1f, 0.85f, 0.33f, true);
  models_[static_cast<int>(VehicleType::Van)] = makeCar(4.7f, 1.7f, 1.98f, 1.0f, 2.0f, -2.3f, 0.05f, 0.35f, 0.31f, true);
  models_[static_cast<int>(VehicleType::Truck)] = makeTruck();
  models_[static_cast<int>(VehicleType::Bus)] = makeBus();
  // Wheel: cylinder along x (width 0.2 m), built once per radius class at draw via scale.
  for (auto& m : models_) {
    Mesh w = GenMeshCylinder(1.0f, 1.0f, 14);  // unit, axis +y; oriented at draw time
    m.wheel = w;
  }
  ready_ = true;
}

void VehicleModels::unload() {
  if (!ready_) return;
  for (auto& m : models_)
    for (Mesh* x : {&m.body, &m.wheel, &m.head_lamps, &m.tail_lamps})
      if (x->vaoId) {
        UnloadMesh(*x);
        *x = Mesh{};
      }
  ready_ = false;
}

}  // namespace rjc
