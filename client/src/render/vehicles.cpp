#include "render/vehicles.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/driving.hpp"
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
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a));
    if (Vector3Length(n) < 1e-9f) n = Vector3CrossProduct(Vector3Subtract(c, b), Vector3Subtract(a, b));
    n = Vector3Normalize(n);
    const unsigned short k = v();
    vert(a, n, col, mat);
    vert(b, n, col, mat);
    vert(c, n, col, mat);
    vert(d, n, col, mat);
    tri(k, static_cast<unsigned short>(k + 1), static_cast<unsigned short>(k + 2));
    tri(k, static_cast<unsigned short>(k + 2), static_cast<unsigned short>(k + 3));
  }
  // Oriented box: centre c, half extents h along the axes (ax, ay, az) (right-handed).
  void obox(Vector3 c, Vector3 h, Vector3 ax, Vector3 ay, Vector3 az, Color col, int mat) {
    auto P = [&](float sx, float sy, float sz) {
      return Vector3Add(c, Vector3Add(Vector3Scale(ax, sx * h.x), Vector3Add(Vector3Scale(ay, sy * h.y), Vector3Scale(az, sz * h.z))));
    };
    const Vector3 p[8] = {P(-1, -1, -1), P(1, -1, -1), P(1, 1, -1), P(-1, 1, -1), P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1)};
    quad(p[4], p[5], p[6], p[7], col, mat);
    quad(p[3], p[2], p[1], p[0], col, mat);
    quad(p[0], p[1], p[5], p[4], col, mat);
    quad(p[2], p[3], p[7], p[6], col, mat);
    quad(p[1], p[2], p[6], p[5], col, mat);
    quad(p[3], p[0], p[4], p[7], col, mat);
  }
  void box(Vector3 c, Vector3 h, Color col, int mat) { obox(c, h, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, col, mat); }
  // Beam from a to b: width w (sideways), thickness t (perpendicular, towards `up`).
  void beam(Vector3 a, Vector3 b, float w, float t, Color col, int mat, Vector3 up = {0, 0, 1}) {
    Vector3 d = Vector3Subtract(b, a);
    const float len = Vector3Length(d);
    if (len < 1e-5f) return;
    d = Vector3Scale(d, 1.0f / len);
    Vector3 s = Vector3CrossProduct(d, up);
    if (Vector3Length(s) < 1e-4f) s = Vector3CrossProduct(d, Vector3{1, 0, 0});
    s = Vector3Normalize(s);
    const Vector3 u = Vector3CrossProduct(s, d);
    obox(Vector3Scale(Vector3Add(a, b), 0.5f), {w * 0.5f, len * 0.5f, t * 0.5f}, s, d, u, col, mat);
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
const Color kWell{14, 14, 15, 255};
const Color kChrome{205, 207, 210, 255};

// ---------------------------------------------------------------------------------------------
// Lower body: cross-sections along y (forward), sampled densely, with wheel-arch openings.
struct Sec {
  float y, zb, zt, wb, wt;
};
Sec sample(const std::vector<Sec>& s, float y) {
  if (y <= s.front().y) return s.front();
  for (size_t i = 1; i < s.size(); ++i)
    if (y <= s[i].y) {
      const float t = (y - s[i - 1].y) / std::max(1e-5f, s[i].y - s[i - 1].y);
      auto L = [&](float a, float b) { return a + (b - a) * t; };
      return {y, L(s[i - 1].zb, s[i].zb), L(s[i - 1].zt, s[i].zt), L(s[i - 1].wb, s[i].wb), L(s[i - 1].wt, s[i].wt)};
    }
  return s.back();
}

struct Arch {
  float y, r, zc;  // wheel centre y, opening radius, centre height
};

void lowerBody(Geo& g, const std::vector<Sec>& prof, const std::vector<Arch>& arches) {
  std::vector<float> ys;
  for (float y = prof.front().y; y < prof.back().y; y += 0.05f) ys.push_back(y);
  ys.push_back(prof.back().y);
  for (const auto& a : arches)  // exact arch ends
    for (float e : {a.y - a.r, a.y + a.r}) ys.push_back(e);
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end(), [](float a, float b) { return std::fabs(a - b) < 0.004f; }), ys.end());
  auto zbot = [&](float y, const Sec& s) {
    float z = s.zb;
    for (const auto& a : arches) {
      const float dy = y - a.y;
      if (std::fabs(dy) < a.r) z = std::max(z, a.zc + std::sqrt(std::max(0.0f, a.r * a.r - dy * dy)));
    }
    return z;
  };
  for (size_t i = 0; i + 1 < ys.size(); ++i) {
    const Sec A = sample(prof, ys[i]), B = sample(prof, ys[i + 1]);
    const float za = zbot(A.y, A), zb = zbot(B.y, B);
    // shoulder line 0.13 m below the top: vertical flank below, tumblehome above
    const float sa = std::max(za + 0.01f, A.zt - 0.13f), sb = std::max(zb + 0.01f, B.zt - 0.13f);
    for (float sg : {1.0f, -1.0f}) {
      auto P = [&](float w, float y, float z) { return Vector3{sg * w, y, z}; };
      if (sg > 0) {
        g.quad(P(A.wb, A.y, za), P(B.wb, B.y, zb), P(B.wb, B.y, sb), P(A.wb, A.y, sa), kPaint, kMatCarPaint);
        g.quad(P(A.wb, A.y, sa), P(B.wb, B.y, sb), P(B.wt, B.y, B.zt), P(A.wt, A.y, A.zt), kPaint, kMatCarPaint);
      } else {
        g.quad(P(B.wb, B.y, zb), P(A.wb, A.y, za), P(A.wb, A.y, sa), P(B.wb, B.y, sb), kPaint, kMatCarPaint);
        g.quad(P(B.wb, B.y, sb), P(A.wb, A.y, sa), P(A.wt, A.y, A.zt), P(B.wt, B.y, B.zt), kPaint, kMatCarPaint);
      }
    }
    // top (bonnet / boot / under the cabin)
    g.quad({A.wt, A.y, A.zt}, {B.wt, B.y, B.zt}, {-B.wt, B.y, B.zt}, {-A.wt, A.y, A.zt}, kPaint, kMatCarPaint);
    // underside, kept inside the wheels
    g.quad({-(A.wb - 0.28f), A.y, A.zb}, {-(B.wb - 0.28f), B.y, B.zb}, {B.wb - 0.28f, B.y, B.zb}, {A.wb - 0.28f, A.y, A.zb}, kTrim, kMatTyre);
    // wheel wells: arch lining and the inner wall
    if (za > A.zb + 0.01f || zb > B.zb + 0.01f) {
      for (float sg : {1.0f, -1.0f}) {
        const float xo = sg * A.wb, xi = sg * (A.wb - 0.27f), xo2 = sg * B.wb, xi2 = sg * (B.wb - 0.27f);
        g.quad({xo, A.y, za - 0.002f}, {xi, A.y, za - 0.002f}, {xi2, B.y, zb - 0.002f}, {xo2, B.y, zb - 0.002f}, kWell, kMatTyre);
        g.quad({xi, A.y, A.zb}, {xi2, B.y, B.zb}, {xi2, B.y, zb}, {xi, A.y, za}, kWell, kMatTyre);
      }
    }
  }
  // end caps
  for (int e = 0; e < 2; ++e) {
    const Sec s = e ? prof.back() : prof.front();
    const float z0 = zbot(s.y, s), zs = std::max(z0 + 0.01f, s.zt - 0.13f);
    const Vector3 p[6] = {{-s.wb, s.y, z0}, {s.wb, s.y, z0}, {s.wb, s.y, zs}, {s.wt, s.y, s.zt}, {-s.wt, s.y, s.zt}, {-s.wb, s.y, zs}};
    if (e) {
      g.quad(p[1], p[0], p[5], p[2], kPaint, kMatCarPaint);
      g.quad(p[2], p[5], p[4], p[3], kPaint, kMatCarPaint);
    } else {
      g.quad(p[0], p[1], p[2], p[5], kPaint, kMatCarPaint);
      g.quad(p[5], p[2], p[3], p[4], kPaint, kMatCarPaint);
    }
  }
}

// Greenhouse: glass sides, painted roof and pillars, sloped windscreen / rear window (glass).
void cabin(Geo& g, float y0, float y1, float zbelt, float zroof, float rear_slope, float front_slope, float wb, float wt, bool sedan,
           Color trim) {
  const float yr = y0 + rear_slope, yf = y1 - front_slope;
  g.quad({wb, y0, zbelt}, {wb, y1, zbelt}, {wt, yf, zroof}, {wt, yr, zroof}, kPaint, kMatGlass);
  g.quad({-wb, y1, zbelt}, {-wb, y0, zbelt}, {-wt, yr, zroof}, {-wt, yf, zroof}, kPaint, kMatGlass);
  g.quad({wb, y1, zbelt}, {-wb, y1, zbelt}, {-wt, yf, zroof}, {wt, yf, zroof}, kPaint, kMatGlass);
  g.quad({-wb, y0, zbelt}, {wb, y0, zbelt}, {wt, yr, zroof}, {-wt, yr, zroof}, kPaint, kMatGlass);
  // roof with rounded edges
  const float bev = 0.05f;
  g.quad({wt - bev, yr + 0.03f, zroof + 0.02f}, {wt - bev, yf - 0.03f, zroof + 0.02f}, {-(wt - bev), yf - 0.03f, zroof + 0.02f},
         {-(wt - bev), yr + 0.03f, zroof + 0.02f}, kPaint, kMatCarPaint);
  for (float sg : {1.0f, -1.0f}) {
    if (sg > 0)
      g.quad({wt, yr, zroof}, {wt, yf, zroof}, {wt - bev, yf - 0.03f, zroof + 0.02f}, {wt - bev, yr + 0.03f, zroof + 0.02f}, kPaint, kMatCarPaint);
    else
      g.quad({-wt, yf, zroof}, {-wt, yr, zroof}, {-(wt - bev), yr + 0.03f, zroof + 0.02f}, {-(wt - bev), yf - 0.03f, zroof + 0.02f}, kPaint,
             kMatCarPaint);
  }
  g.quad({wt, yf, zroof}, {-wt, yf, zroof}, {-(wt - bev), yf - 0.03f, zroof + 0.02f}, {wt - bev, yf - 0.03f, zroof + 0.02f}, kPaint, kMatCarPaint);
  g.quad({-wt, yr, zroof}, {wt, yr, zroof}, {wt - bev, yr + 0.03f, zroof + 0.02f}, {-(wt - bev), yr + 0.03f, zroof + 0.02f}, kPaint, kMatCarPaint);
  const float eps = 0.004f;
  for (float sg : {1.0f, -1.0f}) {
    auto Q = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col, int mat) {
      if (sg > 0) g.quad(a, b, c, d, col, mat);
      else g.quad({-b.x, b.y, b.z}, {-a.x, a.y, a.z}, {-d.x, d.y, d.z}, {-c.x, c.y, c.z}, col, mat);
    };
    const float xb = wb + eps, xt = wt + eps;
    // A-pillar (body colour) along the windscreen edge
    Q({xb, y1 - 0.13f, zbelt}, {xb, y1, zbelt}, {xt, yf, zroof}, {xt, yf - 0.09f, zroof}, kPaint, kMatCarPaint);
    // C-pillar: sedans have a wide painted pillar, boxy cars a narrow D-pillar
    const float cw = sedan ? 0.42f : 0.12f;
    Q({xb, y0, zbelt}, {xb, y0 + cw + rear_slope * 0.6f, zbelt}, {xt, yr + cw * 0.6f, zroof}, {xt, yr, zroof}, kPaint, kMatCarPaint);
    // B-pillar (black) and the window sill trim
    const float ym = (yr + yf) * 0.5f + 0.1f;
    Q({xb, ym - 0.05f, zbelt}, {xb, ym + 0.05f, zbelt}, {xt, ym + 0.05f, zroof}, {xt, ym - 0.05f, zroof}, kTrim, kMatTyre);
    Q({xb, y0, zbelt - 0.005f}, {xb, y1, zbelt - 0.005f}, {xb, y1, zbelt + 0.02f}, {xb, y0, zbelt + 0.02f}, trim, trim.r > 100 ? kMatMetal : kMatTyre);
  }
  // wipers parked at the base of the windscreen
  g.beam({-0.62f, y1 + 0.02f, zbelt + 0.015f}, {0.05f, y1 + 0.03f, zbelt + 0.02f}, 0.02f, 0.012f, kTrim, kMatTyre);
  g.beam({0.02f, y1 + 0.02f, zbelt + 0.015f}, {0.68f, y1 + 0.03f, zbelt + 0.02f}, 0.02f, 0.012f, kTrim, kMatTyre);
}

void plates(Geo& g, float yf, float yr, float z, float zr) {
  const Color white{235, 235, 228, 255};
  g.quad({-0.165f, yf + 0.01f, z - 0.08f}, {0.165f, yf + 0.01f, z - 0.08f}, {0.165f, yf + 0.01f, z + 0.08f}, {-0.165f, yf + 0.01f, z + 0.08f},
         white, kMatUntinted);
  g.quad({0.165f, yr - 0.01f, zr - 0.08f}, {-0.165f, yr - 0.01f, zr - 0.08f}, {-0.165f, yr - 0.01f, zr + 0.08f}, {0.165f, yr - 0.01f, zr + 0.08f},
         white, kMatUntinted);
}

void mirrors(Geo& g, float y, float z, float w) {
  for (float sg : {1.0f, -1.0f}) {
    g.box({sg * (w + 0.1f), y, z}, {0.08f, 0.05f, 0.06f}, kPaint, kMatCarPaint);
    g.box({sg * (w + 0.03f), y + 0.02f, z - 0.03f}, {0.03f, 0.03f, 0.02f}, kTrim, kMatTyre);  // arm
    g.quad({sg * (w + 0.17f), y - 0.052f, z - 0.045f}, {sg * (w + 0.03f), y - 0.052f, z - 0.045f}, {sg * (w + 0.03f), y - 0.052f, z + 0.045f},
           {sg * (w + 0.17f), y - 0.052f, z + 0.045f}, kPaint, kMatGlass);
  }
}

// ---------------------------------------------------------------------------------------------
// Wheel: tyre with tread and sidewalls, five-spoke alloy rim, brake disc behind the spokes.
Mesh makeWheel(float r, float w, bool steel) {
  Geo g;
  const int N = 22;
  const Color tyre{28, 28, 30, 255}, silver = steel ? Color{150, 152, 156, 255} : Color{196, 198, 202, 255};
  auto ring = [&](float x, float r0, float r1, bool outward, Color c, int mat) {  // annulus facing +x (outward) or -x
    for (int k = 0; k < N; ++k) {
      const float a0 = 2 * PI * k / N, a1 = 2 * PI * (k + 1) / N;
      const Vector3 p0{x, r0 * std::cos(a0), r0 * std::sin(a0)}, p1{x, r0 * std::cos(a1), r0 * std::sin(a1)};
      const Vector3 q0{x, r1 * std::cos(a0), r1 * std::sin(a0)}, q1{x, r1 * std::cos(a1), r1 * std::sin(a1)};
      if (outward) g.quad(p0, q0, q1, p1, c, mat);
      else g.quad(p1, q1, q0, p0, c, mat);
    }
  };
  // tread (slightly rounded shoulders)
  const float rs = r - 0.025f;
  for (int k = 0; k < N; ++k) {
    const float a0 = 2 * PI * k / N, a1 = 2 * PI * (k + 1) / N;
    auto P = [&](float x, float rr, float a) { return Vector3{x, rr * std::cos(a), rr * std::sin(a)}; };
    const float xs[4] = {-w / 2, -w / 2 + 0.03f, w / 2 - 0.03f, w / 2};
    const float rr[4] = {rs, r, r, rs};
    for (int j = 0; j < 3; ++j) g.quad(P(xs[j], rr[j], a0), P(xs[j], rr[j], a1), P(xs[j + 1], rr[j + 1], a1), P(xs[j + 1], rr[j + 1], a0), tyre, kMatTyre);
  }
  const float rim = r * 0.66f;
  ring(w / 2, rim, rs, true, tyre, kMatTyre);     // outer sidewall
  ring(-w / 2, rim, rs, false, tyre, kMatTyre);   // inner sidewall
  ring(-w / 2 + 0.01f, 0.0f, rim, false, Color{30, 30, 32, 255}, kMatTyre);
  // rim: lip, dark barrel behind the spokes, brake disc, spokes, hub
  ring(w / 2 - 0.004f, rim * 0.9f, rim, true, silver, kMatMetal);
  ring(w / 2 - 0.08f, 0.0f, rim * 0.9f, true, Color{26, 26, 28, 255}, kMatTyre);
  ring(w / 2 - 0.065f, rim * 0.25f, rim * 0.78f, true, Color{110, 108, 104, 255}, kMatMetalDark);
  const int spokes = steel ? 8 : 5;
  for (int k = 0; k < spokes; ++k) {
    const float a = 2 * PI * k / spokes + 0.3f;
    const Vector3 d{0, std::cos(a), std::sin(a)};
    const Vector3 a0 = Vector3Add({w / 2 - 0.03f, 0, 0}, Vector3Scale(d, rim * 0.2f));
    const Vector3 a1 = Vector3Add({w / 2 - 0.045f, 0, 0}, Vector3Scale(d, rim * 0.92f));
    if (steel) g.beam(a0, a1, rim * 0.16f, 0.02f, silver, kMatMetal, {1, 0, 0});
    else g.beam(a0, a1, rim * 0.28f, 0.03f, silver, kMatMetal, {1, 0, 0});
  }
  ring(w / 2 - 0.018f, 0.0f, rim * 0.26f, true, silver, kMatMetal);
  ring(w / 2 - 0.014f, 0.0f, rim * 0.1f, true, Color{40, 40, 44, 255}, kMatTyre);
  return g.upload();
}

// ---------------------------------------------------------------------------------------------
// First-person cockpit (right-hand drive).
struct CockpitSpec {
  float hw;                     // half width at the doors
  float ws_base_y, ws_base_z;   // windscreen lower edge
  float ws_top_y, ws_top_z;     // windscreen upper edge (roof front)
  float roof_z, rear_y, belt_z;
  float nose_y, nose_z;         // bonnet leading edge (nose_y <= ws_base_y: no bonnet in view)
  float wheel_r = 0.185f, tilt = 0.45f;
  bool rear_seats = true;
};

void makeCockpit(VehicleModel& m, VehicleType type, const CockpitSpec& c) {
  Geo g, bon, gau;
  const DriverSeat st = driverSeat(type);
  const float ex = st.side, ey = st.fwd, ez = st.up;
  const Color dark{36, 36, 38, 255}, mid{66, 66, 68, 255}, lining{150, 146, 138, 255}, seat{48, 48, 52, 255}, carpet{30, 30, 32, 255};
  const float dz = c.ws_base_z - 0.03f;         // dashboard top (just under the windscreen base)
  const float dash_back = ey + 0.62f;           // dashboard face towards the driver
  const float hw = c.hw - 0.04f;
  // dashboard: upper slab, lower knee panel, centre stack with a (dark glass) screen
  g.box({0, (c.ws_base_y + dash_back) * 0.5f, dz - 0.18f}, {hw, (c.ws_base_y - dash_back) * 0.5f, 0.18f}, dark, kMatUntinted);
  g.box({0, dash_back + 0.1f, dz - 0.5f}, {hw, 0.1f, 0.15f}, mid, kMatUntinted);
  g.quad({-0.11f, dash_back - 0.004f, dz - 0.3f}, {0.11f, dash_back - 0.004f, dz - 0.3f}, {0.11f, dash_back - 0.004f, dz - 0.08f},
         {-0.11f, dash_back - 0.004f, dz - 0.08f}, kPaint, kMatGlass);
  for (float x : {-0.16f, 0.16f}) g.box({x, dash_back - 0.01f, dz - 0.05f}, {0.04f, 0.012f, 0.02f}, mid, kMatUntinted);  // vents
  g.box({-ex, dash_back - 0.01f, dz - 0.05f}, {0.05f, 0.012f, 0.02f}, mid, kMatUntinted);
  // instrument cluster: face below a hood, two dials
  const float fy = dash_back - 0.02f;
  g.quad({ex - 0.18f, fy, dz - 0.14f}, {ex + 0.18f, fy, dz - 0.14f}, {ex + 0.18f, fy, dz + 0.02f}, {ex - 0.18f, fy, dz + 0.02f}, Color{12, 12, 14, 255},
         kMatUntinted);
  g.obox({ex, fy + 0.05f, dz + 0.035f}, {0.2f, 0.08f, 0.012f}, {1, 0, 0}, {0, 0.97f, -0.24f}, {0, 0.24f, 0.97f}, dark, kMatUntinted);  // hood
  for (float sx : {-1.0f, 1.0f}) g.box({ex + sx * 0.195f, fy + 0.04f, dz - 0.05f}, {0.01f, 0.06f, 0.085f}, dark, kMatUntinted);
  const float gz = dz - 0.065f, gr = 0.055f;
  m.gauge_pos[0][0] = ex - 0.085f, m.gauge_pos[0][1] = fy - 0.002f, m.gauge_pos[0][2] = gz;
  m.gauge_pos[1][0] = ex + 0.085f, m.gauge_pos[1][1] = fy - 0.002f, m.gauge_pos[1][2] = gz;
  for (int k = 0; k < 2; ++k) {
    const float cx = m.gauge_pos[k][0];
    const int majors = k == 0 ? 10 : 9;  // 0-180 km/h by 20, 0-8 x1000 rpm
    for (int i = 0; i <= (majors - 1) * 2; ++i) {
      const float t = static_cast<float>(i) / ((majors - 1) * 2);
      const float a = (-120.0f + 240.0f * t) * DEG2RAD;
      const bool major = i % 2 == 0;
      const float r0 = gr * (major ? 0.78f : 0.86f);
      const Vector3 d{std::sin(a), 0, std::cos(a)}, s{std::cos(a), 0, -std::sin(a)};
      const float hw2 = major ? 0.0022f : 0.0012f;
      const Vector3 base{cx, fy - 0.003f, gz};
      const Vector3 p0 = Vector3Add(base, Vector3Scale(d, r0)), p1 = Vector3Add(base, Vector3Scale(d, gr));
      gau.quad(Vector3Subtract(p0, Vector3Scale(s, hw2)), Vector3Add(p0, Vector3Scale(s, hw2)), Vector3Add(p1, Vector3Scale(s, hw2)),
               Vector3Subtract(p1, Vector3Scale(s, hw2)), WHITE, kMatSignalLamp);
    }
    for (int i = 0; i < 28; ++i) {  // outer ring
      const float a0 = (-125.0f + 250.0f * i / 28.0f) * DEG2RAD, a1 = (-125.0f + 250.0f * (i + 1) / 28.0f) * DEG2RAD;
      const Vector3 base{cx, fy - 0.003f, gz};
      auto P = [&](float a, float r) { return Vector3Add(base, Vector3{std::sin(a) * r, 0, std::cos(a) * r}); };
      gau.quad(P(a0, gr), P(a1, gr), P(a1, gr * 1.04f), P(a0, gr * 1.04f), WHITE, kMatSignalLamp);
    }
  }
  {
    Geo nd;  // needle in the x-z plane, pointing +z, facing -y
    nd.quad({-0.0022f, -0.002f, -0.012f}, {0.0022f, -0.002f, -0.012f}, {0.0008f, -0.002f, gr * 0.92f}, {-0.0008f, -0.002f, gr * 0.92f}, WHITE,
            kMatSignalLamp);
    m.needle = nd.upload();
  }
  // steering wheel (plane x-z, axis y): rim, three spokes, hub
  {
    Geo sw;
    const float R = c.wheel_r;
    const int N = 28;
    for (int k = 0; k < N; ++k) {
      const float a0 = 2 * PI * k / N, a1 = 2 * PI * (k + 1) / N;
      sw.beam({R * std::sin(a0), 0, R * std::cos(a0)}, {R * std::sin(a1), 0, R * std::cos(a1)}, 0.03f, 0.03f, Color{24, 24, 26, 255}, kMatUntinted,
              {0, 1, 0});
    }
    for (float a : {PI * 0.5f, -PI * 0.5f, PI})
      sw.beam({0.04f * std::sin(a), 0.01f, 0.04f * std::cos(a)}, {R * std::sin(a), 0, R * std::cos(a)}, 0.05f, 0.016f, Color{44, 44, 46, 255},
              kMatUntinted, {0, 1, 0});
    sw.box({0, 0.02f, -0.01f}, {0.065f, 0.03f, 0.055f}, Color{34, 34, 36, 255}, kMatUntinted);
    sw.box({0, -0.012f, 0.0f}, {0.02f, 0.004f, 0.01f}, kChrome, kMatMetal);  // emblem plate (generic)
    m.steering = sw.upload();
    // low enough that the dials show above the rim
    m.steer_pos[0] = ex, m.steer_pos[1] = ey + 0.42f, m.steer_pos[2] = ez - 0.45f;
    m.steer_tilt = c.tilt;
    // column shroud from the wheel to the dashboard
    g.beam({ex, ey + 0.47f, ez - 0.48f}, {ex, dash_back + 0.05f, dz - 0.25f}, 0.08f, 0.08f, dark, kMatUntinted);
  }
  // A-pillars, header, roof lining, sun visors, rear-view mirror
  for (float sg : {1.0f, -1.0f}) {
    g.beam({sg * (c.hw - 0.03f), c.ws_base_y, c.ws_base_z}, {sg * (c.hw - 0.14f), c.ws_top_y, c.ws_top_z}, 0.1f, 0.06f, Color{58, 58, 60, 255},
           kMatUntinted);
    g.box({sg * 0.36f, c.ws_top_y - 0.14f, c.roof_z - 0.06f}, {0.17f, 0.1f, 0.012f}, lining, kMatUntinted);
  }
  g.box({0, c.ws_top_y - 0.03f, c.roof_z - 0.03f}, {c.hw - 0.1f, 0.05f, 0.03f}, lining, kMatUntinted);
  g.quad({-(c.hw - 0.08f), c.rear_y, c.roof_z - 0.02f}, {c.hw - 0.08f, c.rear_y, c.roof_z - 0.02f}, {c.hw - 0.08f, c.ws_top_y, c.roof_z - 0.02f},
         {-(c.hw - 0.08f), c.ws_top_y, c.roof_z - 0.02f}, lining, kMatUntinted);
  // rear-view mirror hanging from the windscreen header (glass faces the driver)
  const float my = c.ws_top_y + 0.02f, mz = c.roof_z - 0.1f;
  g.box({0, my, mz}, {0.11f, 0.016f, 0.032f}, dark, kMatUntinted);
  g.quad({0.1f, my - 0.017f, mz - 0.026f}, {-0.1f, my - 0.017f, mz - 0.026f}, {-0.1f, my - 0.017f, mz + 0.026f}, {0.1f, my - 0.017f, mz + 0.026f}, kPaint,
         kMatGlass);
  auto setGlass = [&](int k, float x0, float x1, float y, float z0, float z1) {  // seen from behind (-y): x0 left, x1 right
    const float c4[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
    std::copy(&c4[0][0], &c4[0][0] + 12, &m.mirror_glass[k][0][0]);
  };
  setGlass(0, -0.1f, 0.1f, my - 0.019f, mz - 0.026f, mz + 0.026f);
  g.beam({0, my + 0.01f, mz + 0.03f}, {0, my + 0.05f, c.roof_z - 0.02f}, 0.02f, 0.02f, dark, kMatUntinted);
  // doors, sills, B-pillars, floor
  const float yb = ey - 0.28f;
  for (float sg : {1.0f, -1.0f}) {
    const float x = sg * (c.hw - 0.02f);
    if (sg > 0) g.quad({x, c.rear_y, 0.35f}, {x, c.ws_base_y, 0.35f}, {x, c.ws_base_y, c.belt_z}, {x, c.rear_y, c.belt_z}, mid, kMatUntinted);
    else g.quad({x, c.ws_base_y, 0.35f}, {x, c.rear_y, 0.35f}, {x, c.rear_y, c.belt_z}, {x, c.ws_base_y, c.belt_z}, mid, kMatUntinted);
    g.box({sg * (c.hw - 0.06f), (c.rear_y + c.ws_base_y) * 0.5f, c.belt_z - 0.01f}, {0.05f, (c.ws_base_y - c.rear_y) * 0.5f, 0.02f}, dark, kMatUntinted);
    g.box({sg * (c.hw - 0.06f), (c.rear_y + c.ws_base_y) * 0.5f, c.belt_z - 0.2f}, {0.03f, (c.ws_base_y - c.rear_y) * 0.4f, 0.03f}, dark,
          kMatUntinted);  // armrest
    g.box({sg * (c.hw - 0.05f), yb, (c.belt_z + c.roof_z) * 0.5f}, {0.04f, 0.07f, (c.roof_z - c.belt_z) * 0.5f}, lining, kMatUntinted);
  }
  g.quad({-c.hw, c.rear_y, 0.3f}, {c.hw, c.rear_y, 0.3f}, {c.hw, c.ws_base_y, 0.3f}, {-c.hw, c.ws_base_y, 0.3f}, carpet, kMatUntinted);
  // centre console with the selector
  g.box({0, ey + 0.15f, ez - 0.72f}, {0.11f, 0.42f, 0.14f}, mid, kMatUntinted);
  g.box({0.02f, ey + 0.25f, ez - 0.55f}, {0.025f, 0.03f, 0.05f}, dark, kMatUntinted);
  // passenger seat and the rear bench
  const float px = -ex;
  g.box({px, ey - 0.02f, ez - 0.68f}, {0.25f, 0.26f, 0.07f}, seat, kMatUntinted);
  g.obox({px, ey - 0.3f, ez - 0.3f}, {0.25f, 0.06f, 0.32f}, {1, 0, 0}, {0, 0.97f, 0.24f}, {0, -0.24f, 0.97f}, seat, kMatUntinted);
  g.box({px, ey - 0.4f, ez + 0.09f}, {0.13f, 0.05f, 0.1f}, seat, kMatUntinted);
  if (c.rear_seats && c.rear_y < ey - 1.1f) {
    g.box({0, ey - 0.95f, ez - 0.66f}, {c.hw - 0.12f, 0.24f, 0.08f}, seat, kMatUntinted);
    g.box({0, ey - 1.22f, ez - 0.3f}, {c.hw - 0.12f, 0.06f, 0.3f}, seat, kMatUntinted);
  }
  // painted parts seen from the seat: bonnet (slight crown), wing tops, mirror housings
  if (c.nose_y > c.ws_base_y + 0.2f) {
    const float xs[4] = {-(c.hw + 0.03f), -0.35f, 0.35f, c.hw + 0.03f};
    const float cr[4] = {0.0f, 0.025f, 0.025f, 0.0f};
    for (int i = 0; i < 3; ++i)
      bon.quad({xs[i], c.ws_base_y, c.ws_base_z - 0.02f + cr[i]}, {xs[i + 1], c.ws_base_y, c.ws_base_z - 0.02f + cr[i + 1]},
               {xs[i + 1] * 0.94f, c.nose_y, c.nose_z + cr[i + 1]}, {xs[i] * 0.94f, c.nose_y, c.nose_z + cr[i]}, kPaint, kMatCarPaint);
  }
  for (float sg : {1.0f, -1.0f}) {
    const float y = c.ws_base_y - 0.12f, z = c.belt_z + 0.08f;
    bon.box({sg * (c.hw + 0.13f), y, z}, {0.08f, 0.05f, 0.06f}, kPaint, kMatCarPaint);
    g.quad({sg * (c.hw + 0.2f), y - 0.052f, z - 0.045f}, {sg * (c.hw + 0.06f), y - 0.052f, z - 0.045f}, {sg * (c.hw + 0.06f), y - 0.052f, z + 0.045f},
           {sg * (c.hw + 0.2f), y - 0.052f, z + 0.045f}, kPaint, kMatGlass);
    const float xa = sg * (c.hw + 0.06f), xb = sg * (c.hw + 0.2f);
    setGlass(sg > 0 ? 1 : 2, std::min(xa, xb), std::max(xa, xb), y - 0.054f, z - 0.045f, z + 0.045f);
  }
  // wipers
  g.beam({-0.62f, c.ws_base_y + 0.03f, c.ws_base_z + 0.012f}, {0.05f, c.ws_base_y + 0.04f, c.ws_base_z + 0.018f}, 0.02f, 0.012f, kTrim, kMatUntinted);
  g.beam({0.02f, c.ws_base_y + 0.03f, c.ws_base_z + 0.012f}, {0.68f, c.ws_base_y + 0.04f, c.ws_base_z + 0.018f}, 0.02f, 0.012f, kTrim, kMatUntinted);
  m.cockpit = g.upload();
  m.bonnet = bon.upload();
  m.gauges = gau.upload();
}

// ---------------------------------------------------------------------------------------------
struct CarParams {
  float L, W, zroof, zbelt, hood_y, trunk_y, rear_slope, front_slope, wheel_r, fo, ro;
  bool boxy, taxi, sliding;
};

VehicleModel makeCar(VehicleType type, const CarParams& p) {
  Geo body, head, tail;
  const float hw = p.W * 0.5f, yf = p.L * 0.5f, yr = -p.L * 0.5f;
  const float zb = 0.2f;
  const float znose = p.zbelt - (p.boxy ? 0.06f : 0.16f), ztail = p.zbelt - (p.boxy ? 0.02f : 0.05f);
  // plan-view rounding at the corners, bonnet sloping to the nose, boot / tailgate at the rear
  std::vector<Sec> s = {{yr, zb + 0.14f, ztail - 0.06f, hw - 0.12f, hw - 0.17f},
                        {yr + 0.08f, zb + 0.05f, ztail - 0.01f, hw - 0.05f, hw - 0.1f},
                        {yr + 0.35f, zb, ztail, hw - 0.01f, hw - 0.06f},
                        {p.trunk_y + 0.1f, zb, p.zbelt, hw, hw - 0.05f},
                        {p.hood_y, zb, p.zbelt - 0.01f, hw, hw - 0.05f},
                        {yf - 0.4f, zb, znose + 0.04f, hw - 0.01f, hw - 0.07f},
                        {yf - 0.1f, zb + 0.04f, znose, hw - 0.05f, hw - 0.11f},
                        {yf, zb + 0.16f, znose - 0.07f, hw - 0.13f, hw - 0.19f}};
  const float wyf = yf - p.fo, wyr = yr + p.ro, ra = p.wheel_r + 0.055f;
  lowerBody(body, s, {{wyf, ra, p.wheel_r}, {wyr, ra, p.wheel_r}});
  const Color trim = p.taxi ? kChrome : kTrim;
  cabin(body, p.trunk_y, p.hood_y, p.zbelt, p.zroof, p.rear_slope, p.front_slope, hw - 0.06f, hw - (p.boxy ? 0.12f : 0.22f), p.rear_slope > 0.3f,
        trim);
  // bumpers, side skirts, grille
  body.box({0, yf - 0.06f, zb + 0.13f}, {hw - 0.14f, 0.07f, 0.09f}, kTrim, kMatTyre);
  body.box({0, yr + 0.06f, zb + 0.13f}, {hw - 0.14f, 0.07f, 0.09f}, kTrim, kMatTyre);
  for (float sg : {1.0f, -1.0f})
    body.box({sg * (hw + 0.002f), (wyf + wyr) * 0.5f, zb + 0.04f}, {0.012f, (wyf - wyr) * 0.5f - ra, 0.04f}, kTrim, kMatTyre);
  const float gz0 = zb + 0.3f, gz1 = znose - 0.12f;
  if (gz1 > gz0 + 0.04f) {
    body.quad({-hw * 0.4f, yf + 0.003f, gz0}, {hw * 0.4f, yf + 0.003f, gz0}, {hw * 0.4f, yf + 0.003f, gz1}, {-hw * 0.4f, yf + 0.003f, gz1}, kWell, kMatTyre);
    for (float z = gz0 + 0.03f; z < gz1; z += 0.035f)  // grille bars
      body.box({0, yf + 0.006f, z}, {hw * 0.4f, 0.004f, 0.005f}, p.taxi ? kChrome : Color{40, 40, 42, 255}, p.taxi ? kMatMetal : kMatTyre);
  }
  // door seams and handles (front + rear doors; sliding rear door on boxy vans)
  const float yA = p.hood_y - 0.03f, yB = (p.hood_y + p.trunk_y) * 0.5f + 0.12f, yC = p.trunk_y + (p.sliding ? 0.5f : 0.18f);
  for (float sg : {1.0f, -1.0f}) {
    for (float y : {yA, yB, yC}) {
      const Sec q = sample(s, y);
      const float x = sg * (q.wb + 0.003f);
      const float z0 = zb + 0.08f, z1 = q.zt - 0.13f;
      const float ya = y - 0.004f, yb2 = y + 0.004f;
      if (sg > 0) body.quad({x, ya, z0}, {x, yb2, z0}, {x, yb2, z1}, {x, ya, z1}, kWell, kMatTyre);
      else body.quad({x, yb2, z0}, {x, ya, z0}, {x, ya, z1}, {x, yb2, z1}, kWell, kMatTyre);
    }
    for (float y : {yB - 0.14f, yC - 0.12f}) {
      const Sec q = sample(s, y);
      body.box({sg * (q.wb + 0.012f), y, q.zt - 0.12f}, {0.012f, 0.07f, 0.014f}, p.taxi ? kChrome : kPaint, p.taxi ? kMatMetal : kMatCarPaint);
    }
  }
  plates(body, yf - 0.13f, yr + 0.13f, zb + 0.13f, ztail - 0.25f);
  mirrors(body, p.hood_y - 0.12f, p.zbelt + 0.08f, hw - 0.06f);
  if (p.rear_slope > 0.3f) body.box({0, p.trunk_y + p.rear_slope + 0.12f, p.zroof + 0.045f}, {0.03f, 0.09f, 0.035f}, kPaint, kMatCarPaint);  // antenna fin
  if (p.taxi) {  // roof sign ("andon") and the door-edge lamp strip are typical of Tokyo taxis
    body.box({0, (p.trunk_y + p.hood_y) * 0.5f, p.zroof + 0.1f}, {0.24f, 0.08f, 0.09f}, Color{236, 226, 170, 255}, kMatUntinted);
    body.box({0, (p.trunk_y + p.hood_y) * 0.5f, p.zroof + 0.025f}, {0.2f, 0.07f, 0.02f}, kTrim, kMatTyre);
  }
  // lamps: swept head lamps on the front corners, wide tail lamp units
  const Color c{255, 255, 255, 255};
  const float hz = znose - 0.13f;
  for (float sg : {1.0f, -1.0f}) {
    head.box({sg * (hw - 0.26f), yf - 0.02f, hz}, {0.16f, 0.04f, 0.045f}, c, kMatSignalLamp);
    head.box({sg * (hw - 0.1f), yf - 0.12f, hz + 0.005f}, {0.04f, 0.1f, 0.04f}, c, kMatSignalLamp);
    tail.box({sg * (hw - 0.22f), yr + 0.02f, ztail - 0.12f}, {0.18f, 0.04f, 0.06f}, c, kMatSignalLamp);
    tail.box({sg * (hw - 0.08f), yr + 0.14f, ztail - 0.12f}, {0.035f, 0.12f, 0.055f}, c, kMatSignalLamp);
  }
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = p.wheel_r;
  m.wheel = makeWheel(p.wheel_r, p.wheel_r > 0.3f ? 0.215f : 0.175f, p.boxy && !p.taxi && type == VehicleType::Van);
  const float wx = hw - 0.13f;
  const float wp[4][2] = {{wx, wyf}, {-wx, wyf}, {wx, wyr}, {-wx, wyr}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  CockpitSpec cs;
  cs.hw = hw - 0.06f;
  cs.ws_base_y = p.hood_y, cs.ws_base_z = p.zbelt;
  cs.ws_top_y = p.hood_y - p.front_slope, cs.ws_top_z = p.zroof;
  cs.roof_z = p.zroof - 0.02f, cs.rear_y = p.trunk_y + p.rear_slope, cs.belt_z = p.zbelt;
  cs.nose_y = yf - 0.1f, cs.nose_z = znose;
  makeCockpit(m, type, cs);
  return m;
}

VehicleModel makeBus() {
  Geo body, head, tail;
  const float L = 10.5f, W = 2.49f, hw = W * 0.5f, yf = L * 0.5f, yr = -L * 0.5f;
  const float zb = 0.3f, zwin = 1.15f, zwt = 2.55f, zr = 3.05f;
  const float wyf = yf - 2.2f, wyr = yr + 2.6f, wr = 0.48f;
  std::vector<Sec> lower = {{yr, zb, zwin, hw, hw}, {yf, zb, zwin, hw, hw}};
  lowerBody(body, lower, {{wyf, wr + 0.08f, wr}, {wyr, wr + 0.08f, wr}});
  std::vector<Sec> band = {{yr, zwin, zwt, hw, hw - 0.02f}, {yf - 0.05f, zwin, zwt, hw, hw - 0.02f}};
  for (size_t i = 0; i + 1 < band.size(); ++i) {
    const Sec& a = band[i];
    const Sec& b = band[i + 1];
    body.quad({a.wb, a.y, a.zb}, {b.wb, b.y, b.zb}, {b.wt, b.y, b.zt}, {a.wt, a.y, a.zt}, kPaint, kMatGlass);
    body.quad({-b.wb, b.y, b.zb}, {-a.wb, a.y, a.zb}, {-a.wt, a.y, a.zt}, {-b.wt, b.y, b.zt}, kPaint, kMatGlass);
  }
  body.quad({hw, yf - 0.05f, zwin}, {-hw, yf - 0.05f, zwin}, {-hw + 0.02f, yf - 0.05f, zwt}, {hw - 0.02f, yf - 0.05f, zwt}, kPaint, kMatGlass);
  body.quad({-hw, yr, zwin}, {hw, yr, zwin}, {hw - 0.02f, yr, zwt}, {-hw + 0.02f, yr, zwt}, kPaint, kMatCarPaint);
  std::vector<Sec> roof = {{yr, zwt, zr, hw - 0.02f, hw - 0.12f}, {yf - 0.05f, zwt, zr, hw - 0.02f, hw - 0.12f}};
  lowerBody(body, roof, {});
  const Color stripe{40, 130, 90, 255};
  body.quad({hw + 0.005f, yr, zwin - 0.25f}, {hw + 0.005f, yf, zwin - 0.25f}, {hw + 0.005f, yf, zwin - 0.05f}, {hw + 0.005f, yr, zwin - 0.05f}, stripe,
            kMatUntinted);
  body.quad({-hw - 0.005f, yf, zwin - 0.25f}, {-hw - 0.005f, yr, zwin - 0.25f}, {-hw - 0.005f, yr, zwin - 0.05f}, {-hw - 0.005f, yf, zwin - 0.05f},
            stripe, kMatUntinted);
  for (float y = yr + 1.2f; y < yf - 1.0f; y += 1.45f) {
    body.box({hw + 0.01f, y, (zwin + zwt) * 0.5f}, {0.012f, 0.05f, (zwt - zwin) * 0.5f}, kTrim, kMatTyre);
    body.box({-hw - 0.01f, y, (zwin + zwt) * 0.5f}, {0.012f, 0.05f, (zwt - zwin) * 0.5f}, kTrim, kMatTyre);
  }
  // front and middle doors on the kerb (left) side: dark glazed leaves
  for (float yd : {yf - 0.85f, -0.3f})
    body.quad({-hw - 0.012f, yd + 0.55f, zb + 0.1f}, {-hw - 0.012f, yd - 0.55f, zb + 0.1f}, {-hw - 0.012f, yd - 0.55f, zwt - 0.05f},
              {-hw - 0.012f, yd + 0.55f, zwt - 0.05f}, kPaint, kMatGlass);
  body.box({0, yr + 0.3f, zr + 0.12f}, {0.7f, 1.0f, 0.12f}, Color{200, 200, 196, 255}, kMatUntinted);  // roof AC unit
  body.box({0, yf + 0.02f, zwt + 0.2f}, {0.9f, 0.02f, 0.16f}, Color{20, 20, 20, 255}, kMatUntinted);   // destination display
  plates(body, yf, yr, zb + 0.35f, zb + 0.6f);
  const Color c{255, 255, 255, 255};
  for (float sg : {1.0f, -1.0f}) {
    head.box({sg * (hw - 0.25f), yf - 0.02f, zb + 0.45f}, {0.15f, 0.04f, 0.07f}, c, kMatSignalLamp);
    tail.box({sg * (hw - 0.15f), yr + 0.02f, zb + 0.9f}, {0.1f, 0.04f, 0.25f}, c, kMatSignalLamp);
  }
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = wr;
  m.wheel = makeWheel(wr, 0.28f, true);
  const float wx = hw - 0.25f;
  const float wp[4][2] = {{wx, wyf}, {-wx, wyf}, {wx, wyr}, {-wx, wyr}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  CockpitSpec cs;
  cs.hw = hw - 0.05f;
  cs.ws_base_y = yf - 0.05f, cs.ws_base_z = zwin;
  cs.ws_top_y = yf - 0.1f, cs.ws_top_z = zwt;
  cs.roof_z = 2.8f, cs.rear_y = yr + 0.2f, cs.belt_z = zwin;
  cs.nose_y = 0, cs.nose_z = 0;
  cs.wheel_r = 0.24f, cs.tilt = 0.95f;
  cs.rear_seats = false;
  makeCockpit(m, VehicleType::Bus, cs);
  return m;
}

VehicleModel makeTruck() {
  Geo body, head, tail;
  const float L = 6.2f, W = 1.95f, hw = W * 0.5f, yf = L * 0.5f, yr = -L * 0.5f;
  const float cab_r = yf - 1.75f, wr = 0.38f;
  const float wyf = yf - 1.0f, wyr = yr + 1.4f;
  std::vector<Sec> cab = {{cab_r, 0.45f, 1.35f, hw, hw - 0.04f}, {yf - 0.05f, 0.45f, 1.30f, hw, hw - 0.06f}, {yf, 0.55f, 1.2f, hw - 0.05f, hw - 0.1f}};
  lowerBody(body, cab, {{wyf, wr + 0.06f, wr}});
  cabin(body, cab_r, yf, 1.3f, 2.15f, 0.02f, 0.25f, hw - 0.02f, hw - 0.08f, false, kTrim);
  const Color alu{205, 207, 210, 255};
  body.box({0, (yr + cab_r - 0.1f) * 0.5f, 1.75f}, {hw + 0.05f, (cab_r - 0.1f - yr) * 0.5f, 1.2f}, alu, kMatUntinted);
  for (float y = yr + 0.2f; y < cab_r - 0.2f; y += 0.6f)  // box ribs
    for (float sg : {1.0f, -1.0f}) body.box({sg * (hw + 0.06f), y, 1.75f}, {0.01f, 0.025f, 1.18f}, Color{180, 182, 186, 255}, kMatUntinted);
  body.box({0, (yr + cab_r) * 0.5f, 0.55f}, {hw - 0.15f, (cab_r - yr) * 0.5f, 0.12f}, kTrim, kMatTyre);  // chassis
  for (float sg : {1.0f, -1.0f})                                                                         // rear mudguards
    body.box({sg * (hw - 0.13f), wyr, wr * 2 + 0.08f}, {0.2f, 0.55f, 0.02f}, kTrim, kMatTyre);
  plates(body, yf, yr, 0.6f, 0.62f);
  mirrors(body, yf - 0.15f, 1.75f, hw - 0.02f);
  const Color c{255, 255, 255, 255};
  for (float sg : {1.0f, -1.0f}) {
    head.box({sg * (hw - 0.22f), yf + 0.01f, 0.75f}, {0.13f, 0.04f, 0.06f}, c, kMatSignalLamp);
    tail.box({sg * (hw - 0.18f), yr - 0.02f, 0.7f}, {0.13f, 0.04f, 0.06f}, c, kMatSignalLamp);
  }
  VehicleModel m;
  m.body = body.upload();
  m.head_lamps = head.upload();
  m.tail_lamps = tail.upload();
  m.wheel_r = wr;
  m.wheel = makeWheel(wr, 0.23f, true);
  const float wx = hw - 0.2f;
  const float wp[4][2] = {{wx, wyf}, {-wx, wyf}, {wx, wyr}, {-wx, wyr}};
  std::copy(&wp[0][0], &wp[0][0] + 8, &m.wheel_pos[0][0]);
  CockpitSpec cs;
  cs.hw = hw - 0.04f;
  cs.ws_base_y = yf - 0.02f, cs.ws_base_z = 1.3f;
  cs.ws_top_y = yf - 0.25f, cs.ws_top_z = 2.15f;
  cs.roof_z = 2.12f, cs.rear_y = cab_r, cs.belt_z = 1.3f;
  cs.nose_y = 0, cs.nose_z = 0;
  cs.wheel_r = 0.21f, cs.tilt = 0.75f;
  cs.rear_seats = false;
  makeCockpit(m, VehicleType::Truck, cs);
  return m;
}

}  // namespace

void VehicleModels::build() {
  if (ready_) return;
  //                    L      W      roof   belt   hood_y trunk_y rslope fslope wheel  fo     ro     boxy   taxi   sliding
  models_[static_cast<int>(VehicleType::Sedan)] = makeCar(VehicleType::Sedan, {4.7f, 1.8f, 1.44f, 0.92f, 0.95f, -1.35f, 0.55f, 0.75f, 0.315f, 0.9f, 1.05f, false, false, false});
  models_[static_cast<int>(VehicleType::Taxi)] = makeCar(VehicleType::Taxi, {4.4f, 1.7f, 1.74f, 0.98f, 0.85f, -2.05f, 0.12f, 0.55f, 0.30f, 0.75f, 0.85f, true, true, false});
  models_[static_cast<int>(VehicleType::Kei)] = makeCar(VehicleType::Kei, {3.4f, 1.48f, 1.78f, 0.95f, 0.95f, -1.62f, 0.06f, 0.7f, 0.27f, 0.45f, 0.5f, true, false, true});
  models_[static_cast<int>(VehicleType::Minivan)] = makeCar(VehicleType::Minivan, {4.9f, 1.85f, 1.9f, 1.02f, 1.25f, -2.35f, 0.1f, 0.85f, 0.33f, 0.85f, 1.1f, true, false, true});
  models_[static_cast<int>(VehicleType::Van)] = makeCar(VehicleType::Van, {4.7f, 1.7f, 1.98f, 1.0f, 2.0f, -2.3f, 0.05f, 0.35f, 0.31f, 0.65f, 1.5f, true, false, true});
  models_[static_cast<int>(VehicleType::Truck)] = makeTruck();
  models_[static_cast<int>(VehicleType::Bus)] = makeBus();
  ready_ = true;
}

void VehicleModels::unload() {
  if (!ready_) return;
  for (auto& m : models_)
    for (Mesh* x : {&m.body, &m.wheel, &m.head_lamps, &m.tail_lamps, &m.cockpit, &m.bonnet, &m.steering, &m.gauges, &m.needle})
      if (x->vaoId) {
        UnloadMesh(*x);
        *x = Mesh{};
      }
  ready_ = false;
}

}  // namespace rjc
