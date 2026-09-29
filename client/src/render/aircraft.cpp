#include "render/aircraft.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/deck_layout.hpp"
#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

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
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a));
    if (Vector3Length(n) < 1e-9f) n = Vector3CrossProduct(Vector3Subtract(c, b), Vector3Subtract(a, b));
    if (Vector3Length(n) < 1e-12f) return;
    n = Vector3Normalize(n);
    const unsigned short k = v();
    vert(a, n, col, mat);
    vert(b, n, col, mat);
    vert(c, n, col, mat);
    vert(d, n, col, mat);
    idx.insert(idx.end(), {k, static_cast<unsigned short>(k + 1), static_cast<unsigned short>(k + 2), k, static_cast<unsigned short>(k + 2),
                           static_cast<unsigned short>(k + 3)});
  }
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
  void beam(Vector3 a, Vector3 b, float w, float t, Color col, int mat, Vector3 up = {0, 0, 1}) {
    Vector3 d = Vector3Subtract(b, a);
    const float l = Vector3Length(d);
    if (l < 1e-5f) return;
    d = Vector3Scale(d, 1.0f / l);
    Vector3 s = Vector3CrossProduct(d, up);
    if (Vector3Length(s) < 1e-4f) s = Vector3CrossProduct(d, Vector3{1, 0, 0});
    s = Vector3Normalize(s);
    obox(Vector3Scale(Vector3Add(a, b), 0.5f), {w * 0.5f, l * 0.5f, t * 0.5f}, s, d, Vector3CrossProduct(s, d), col, mat);
  }
  // cylinder along x (wheels), centre c, radius r, width w
  void wheelX(Vector3 c, float r, float w, Color col) {
    const int N = 12;
    for (int k = 0; k < N; ++k) {
      const float a0 = 2 * PI * k / N, a1 = 2 * PI * (k + 1) / N;
      auto P = [&](float x, float a) { return Vector3{c.x + x, c.y + r * std::cos(a), c.z + r * std::sin(a)}; };
      quad(P(-w / 2, a0), P(-w / 2, a1), P(w / 2, a1), P(w / 2, a0), col, kMatTyre);
      quad(P(w / 2, a0), P(w / 2, a1), {c.x + w / 2, c.y, c.z}, {c.x + w / 2, c.y, c.z}, Color{150, 150, 150, 255}, kMatMetal);
      quad(P(-w / 2, a1), P(-w / 2, a0), {c.x - w / 2, c.y, c.z}, {c.x - w / 2, c.y, c.z}, Color{150, 150, 150, 255}, kMatMetal);
    }
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

const Color kWhite{240, 241, 243, 255}, kGrey{196, 198, 202, 255}, kBlue{28, 78, 158, 255}, kDark{30, 32, 36, 255};

// Lifting surface between two stations. Each station: leading-edge point, chord (towards -y),
// thickness along `n`. Winding follows the (span, chord, thickness) handedness.
struct Station {
  Vector3 le;
  float chord, thick;
};
void surface(Geo& g, Station A, Station B, Vector3 n, Color top, Color bottom, bool cap_tip = true) {
  auto pts = [&](const Station& s, Vector3 out[4]) {
    const Vector3 c{0, -s.chord, 0};
    out[0] = s.le;
    out[1] = Vector3Add(Vector3Add(s.le, Vector3Scale(c, 0.3f)), Vector3Scale(n, s.thick * 0.6f));
    out[2] = Vector3Add(s.le, c);
    out[3] = Vector3Add(Vector3Add(s.le, Vector3Scale(c, 0.3f)), Vector3Scale(n, -s.thick * 0.4f));
  };
  Vector3 a[4], b[4];
  pts(A, a);
  pts(B, b);
  const Vector3 span = Vector3Subtract(B.le, A.le);
  const bool flip = Vector3DotProduct(Vector3CrossProduct(span, Vector3{0, -1, 0}), n) > 0;
  auto Q = [&](Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, Color c) {
    if (flip) g.quad(p0, p1, p2, p3, c, kMatUntinted);
    else g.quad(p3, p2, p1, p0, c, kMatUntinted);
  };
  Q(a[0], a[1], b[1], b[0], top);
  Q(a[1], a[2], b[2], b[1], top);
  Q(a[2], a[3], b[3], b[2], bottom);
  Q(a[3], a[0], b[0], b[3], bottom);
  if (cap_tip) {
    if (flip) g.quad(b[0], b[1], b[2], b[3], top, kMatUntinted);
    else g.quad(b[3], b[2], b[1], b[0], top, kMatUntinted);
  }
}
Station mirror(Station s) { return {{-s.le.x, s.le.y, s.le.z}, s.chord, s.thick}; }

// ------------------------------------------------------------------------------------------------
JetModel makeJet() {
  JetModel m;
  Geo fus, wings, gear, cabin, red, green, white, door, stairs;
  // fuselage rings: round body, rounded nose drooping slightly, upswept tail cone
  auto ring = [](float y, float& cz, float& r) {
    if (y > 12.0f) {
      const float t = std::min(1.0f, (y - 12.0f) / 6.0f);
      r = 1.5f * std::sqrt(std::max(0.0f, 1.0f - std::pow(t, 2.2f)));
      cz = -0.28f * t;
    } else if (y < -7.0f) {
      const float t = std::min(1.0f, (-7.0f - y) / 11.0f);
      r = 1.5f * (1.0f - 0.85f * std::pow(t, 1.2f));
      cz = 0.9f * std::pow(t, 1.5f);
    } else {
      r = 1.5f;
      cz = 0.0f;
    }
  };
  const int N = 24;
  const float dy = 0.5f;
  for (float y = -18.0f; y < 18.0f - 1e-3f; y += dy) {
    float c0, r0, c1, r1;
    ring(y, c0, r0);
    ring(y + dy, c1, r1);
    for (int k = 0; k < N; ++k) {
      const float a0 = 2 * PI * k / N, a1 = 2 * PI * (k + 1) / N, am = (a0 + a1) * 0.5f;
      const float s = std::sin(am);
      Color c = kWhite;
      if (s < -0.62f) c = kGrey;                                           // belly
      if (s > -0.08f && s < 0.06f && y > -13.0f && y < 14.5f) c = kBlue;  // cheatline
      auto P = [&](float yy, float cz, float r, float a) { return Vector3{r * std::cos(a), yy, cz + r * std::sin(a)}; };
      // the front left door is an opening (its panel is a mesh of its own, drawn when shut)
      const float zm = r0 * s, ym = y + dy * 0.5f;
      const bool in_door = std::cos(am) < 0 && ym > kJetDoorY0 && ym < kJetDoorY1 && zm > -0.75f && zm < 1.2f;
      (in_door ? door : fus).quad(P(y, c0, r0, a1), P(y, c0, r0, a0), P(y + dy, c1, r1, a0), P(y + dy, c1, r1, a1), c, kMatCarPaint);
    }
  }
  // cabin windows (both sides) and doors
  for (float y = -10.4f; y < 11.0f; y += 0.78f)
    for (float sg : {1.0f, -1.0f}) {
      const float x = sg * (std::sqrt(1.5f * 1.5f - 0.35f * 0.35f) + 0.012f);
      if (sg > 0) fus.quad({x, y - 0.13f, 0.17f}, {x, y + 0.13f, 0.17f}, {x, y + 0.13f, 0.53f}, {x, y - 0.13f, 0.53f}, kWhite, kMatGlass);
      else fus.quad({x, y + 0.13f, 0.17f}, {x, y - 0.13f, 0.17f}, {x, y - 0.13f, 0.53f}, {x, y + 0.13f, 0.53f}, kWhite, kMatGlass);
    }
  for (float yd : {11.9f, -11.6f})
    for (float sg : {1.0f, -1.0f}) {
      const float x = sg * 1.505f;
      fus.box({x, yd - 0.45f, 0.2f}, {0.004f, 0.012f, 0.9f}, Color{150, 152, 156, 255}, kMatUntinted);
      fus.box({x, yd + 0.45f, 0.2f}, {0.004f, 0.012f, 0.9f}, Color{150, 152, 156, 255}, kMatUntinted);
      fus.box({x, yd, 1.1f}, {0.004f, 0.45f, 0.012f}, Color{150, 152, 156, 255}, kMatUntinted);
    }
  // cockpit glazing on the nose
  for (int k = 0; k < 4; ++k) {
    const float a0 = (40.0f + k * 25.0f) * DEG2RAD, a1 = (40.0f + (k + 1) * 25.0f) * DEG2RAD;
    float c0, r0, c1, r1;
    ring(13.5f, c0, r0);
    ring(14.6f, c1, r1);
    auto P = [&](float yy, float cz, float r, float a) { return Vector3{r * 1.01f * std::cos(a), yy, cz + r * 1.01f * std::sin(a)}; };
    fus.quad(P(13.5f, c0, r0, a1), P(13.5f, c0, r0, a0), P(14.6f, c1, r1, a0), P(14.6f, c1, r1, a1), kWhite, kMatGlass);
  }
  // wings: low, swept 24 deg, 5 deg dihedral, winglets
  const Station root{{1.2f, 2.6f, -0.95f}, 5.9f, 0.6f};
  const Station tip{{14.3f, 2.6f - 13.1f * std::tan(24.0f * DEG2RAD), -0.95f + 13.1f * std::tan(5.0f * DEG2RAD)}, 1.45f, 0.14f};
  const Station wl{{tip.le.x + 0.25f, tip.le.y - 1.0f, tip.le.z + 1.7f}, 0.6f, 0.08f};
  for (int side = 0; side < 2; ++side) {
    const Station r0 = side ? mirror(root) : root, t0 = side ? mirror(tip) : tip, w0 = side ? mirror(wl) : wl;
    surface(wings, r0, t0, {0, 0, 1}, kGrey, Color{170, 172, 176, 255}, false);
    surface(wings, {t0.le, t0.chord, t0.thick}, w0, {side ? -1.0f : 1.0f, 0, 0}, kBlue, kBlue);
    // engine: nacelle along y under the wing, pylon
    const float ex = side ? -4.9f : 4.9f, ez = -1.8f;
    const float prof[][2] = {{3.5f, 0.74f}, {3.2f, 0.82f}, {2.4f, 0.84f}, {1.0f, 0.74f}, {0.2f, 0.56f}, {-0.6f, 0.22f}};
    for (int i = 0; i + 1 < 6; ++i)
      for (int k = 0; k < 16; ++k) {
        const float a0 = 2 * PI * k / 16, a1 = 2 * PI * (k + 1) / 16;
        auto P = [&](int j, float a) { return Vector3{ex + prof[j][1] * std::cos(a), prof[j][0], ez + prof[j][1] * std::sin(a)}; };
        wings.quad(P(i, a0), P(i, a1), P(i + 1, a1), P(i + 1, a0), i >= 4 ? Color{120, 120, 124, 255} : kGrey, i >= 4 ? kMatMetal : kMatCarPaint);
      }
    for (int k = 0; k < 16; ++k) {  // fan face
      const float a0 = 2 * PI * k / 16, a1 = 2 * PI * (k + 1) / 16;
      wings.quad({ex + 0.7f * std::cos(a1), 3.42f, ez + 0.7f * std::sin(a1)}, {ex + 0.7f * std::cos(a0), 3.42f, ez + 0.7f * std::sin(a0)}, {ex, 3.3f, ez},
                 {ex, 3.3f, ez}, kDark, kMatMetalDark);
    }
    wings.beam({ex, 1.8f, ez + 0.75f}, {ex, 0.2f, -0.55f}, 0.22f, 0.5f, kGrey, kMatCarPaint, {1, 0, 0});
    // navigation lights on the wing tips (red left, green right), strobes
    (side ? red : green).box({t0.le.x + (side ? -0.05f : 0.05f), t0.le.y - 0.3f, t0.le.z}, {0.06f, 0.1f, 0.05f}, WHITE, kMatSignalLamp);
    white.box({t0.le.x + (side ? -0.05f : 0.05f), t0.le.y - 1.3f, t0.le.z}, {0.05f, 0.08f, 0.05f}, WHITE, kMatSignalLamp);
  }
  // tailplane and fin
  const Station hr{{0.3f, -13.6f, 0.6f}, 3.1f, 0.32f}, ht{{5.3f, -16.6f, 1.0f}, 1.2f, 0.1f};
  surface(wings, hr, ht, {0, 0, 1}, kGrey, kGrey);
  surface(wings, mirror(hr), mirror(ht), {0, 0, 1}, kGrey, kGrey);
  const Station fr{{0, -11.2f, 1.05f}, 5.6f, 0.36f}, ft{{0, -16.3f, 6.3f}, 2.1f, 0.12f};
  surface(wings, fr, ft, {1, 0, 0}, kBlue, kBlue);
  surface(wings, fr, ft, {-1, 0, 0}, kBlue, kBlue);
  red.box({0, -1.0f, 1.53f}, {0.08f, 0.12f, 0.04f}, WHITE, kMatSignalLamp);   // beacons
  red.box({0, -1.0f, -1.53f}, {0.08f, 0.12f, 0.04f}, WHITE, kMatSignalLamp);
  white.box({0, -17.9f, 0.88f}, {0.05f, 0.06f, 0.05f}, WHITE, kMatSignalLamp);  // tail light
  // landing gear (ground at z = -2.4)
  gear.beam({0, 14.2f, -1.2f}, {0, 14.2f, -1.95f}, 0.14f, 0.14f, Color{170, 172, 176, 255}, kMatMetal);
  for (float x : {-0.2f, 0.2f}) gear.wheelX({x, 14.2f, -1.95f}, 0.45f, 0.26f, kDark);
  for (float sg : {1.0f, -1.0f}) {
    gear.beam({sg * 2.85f, -2.2f, -1.0f}, {sg * 2.85f, -2.2f, -1.9f}, 0.2f, 0.2f, Color{170, 172, 176, 255}, kMatMetal);
    for (float dx : {-0.3f, 0.3f}) gear.wheelX({sg * 2.85f + dx, -2.2f, -1.9f}, 0.5f, 0.3f, kDark);
  }
  // cabin interior: floor, walls with window reveals, bins, ceiling, 2+2 seats
  const Color wall{226, 224, 218, 255}, seat{40, 54, 100, 255}, head{210, 212, 216, 255}, carpet{60, 64, 78, 255};
  const float y0 = kJetCabinY0, y1 = kJetCabinY1;
  cabin.quad({-1.35f, y0, -0.72f}, {1.35f, y0, -0.72f}, {1.35f, y1, -0.72f}, {-1.35f, y1, -0.72f}, carpet, kMatUntinted);
  {
    // forward vestibule by the front left door: floor, side walls (the door opening on the left),
    // the bulkhead to the cockpit, the ceiling; the aft bulkhead behind the last row
    const float v0 = y1, v1 = kJetVestibuleY1, zf = kJetFloorZ, zc = 1.28f;
    const Color floor_v{150, 150, 146, 255}, bulk{214, 212, 206, 255};
    cabin.quad({-1.35f, v0, zf}, {1.35f, v0, zf}, {1.35f, v1, zf}, {-1.35f, v1, zf}, floor_v, kMatUntinted);
    cabin.quad({1.4f, v1, zf}, {1.4f, v0, zf}, {1.4f, v0, zc}, {1.4f, v1, zc}, wall, kMatUntinted);
    cabin.quad({-1.4f, v0, zf}, {-1.4f, kJetDoorY0, zf}, {-1.4f, kJetDoorY0, zc}, {-1.4f, v0, zc}, wall, kMatUntinted);
    cabin.quad({-1.4f, kJetDoorY1, zf}, {-1.4f, v1, zf}, {-1.4f, v1, zc}, {-1.4f, kJetDoorY1, zc}, wall, kMatUntinted);
    cabin.quad({-1.4f, kJetDoorY0, 1.18f}, {-1.4f, kJetDoorY1, 1.18f}, {-1.4f, kJetDoorY1, zc}, {-1.4f, kJetDoorY0, zc}, wall, kMatUntinted);
    cabin.quad({-1.4f, v1, zf}, {1.4f, v1, zf}, {1.4f, v1, zc}, {-1.4f, v1, zc}, bulk, kMatUntinted);
    cabin.quad({-1.4f, v1, zc}, {1.4f, v1, zc}, {1.4f, v0, zc}, {-1.4f, v0, zc}, Color{240, 240, 236, 255}, kMatUntinted);
    cabin.quad({1.4f, y0, zf}, {-1.4f, y0, zf}, {-1.4f, y0, zc}, {1.4f, y0, zc}, bulk, kMatUntinted);
    cabin.box({0.9f, v1 - 0.45f, zf + 0.5f}, {0.45f, 0.4f, 0.5f}, Color{200, 202, 206, 255}, kMatMetal);  // galley
    // the door opening's lining through the skin
    const Color lin{200, 200, 196, 255};
    cabin.quad({-1.4f, kJetDoorY0, zf}, {-1.55f, kJetDoorY0, zf}, {-1.55f, kJetDoorY0, 1.18f}, {-1.4f, kJetDoorY0, 1.18f}, lin, kMatUntinted);
    cabin.quad({-1.55f, kJetDoorY1, zf}, {-1.4f, kJetDoorY1, zf}, {-1.4f, kJetDoorY1, 1.18f}, {-1.55f, kJetDoorY1, 1.18f}, lin, kMatUntinted);
    cabin.quad({-1.4f, kJetDoorY0, 1.18f}, {-1.55f, kJetDoorY0, 1.18f}, {-1.55f, kJetDoorY1, 1.18f}, {-1.4f, kJetDoorY1, 1.18f}, lin, kMatUntinted);
    cabin.quad({-1.55f, kJetDoorY0, zf}, {-1.4f, kJetDoorY0, zf}, {-1.4f, kJetDoorY1, zf}, {-1.55f, kJetDoorY1, zf}, floor_v, kMatUntinted);
  }
  {
    // passenger stairs: a wheeled stair unit against the door, from the apron (z = -2.4) to the sill
    const Color st{200, 200, 204, 255}, rail{230, 190, 40, 255};
    const float xs = -kJetSkinX - 0.05f, rise = kJetFloorZ + 2.4f;
    const int n = 7;
    const float run = kJetStairRun, ym = (kJetDoorY0 + kJetDoorY1) * 0.5f, hw = 0.5f;
    stairs.box({xs - 0.4f, ym, kJetFloorZ - 0.06f}, {0.4f, hw + 0.05f, 0.06f}, st, kMatMetal);  // top landing
    for (int i = 0; i < n; ++i) {
      const float x = xs - 0.8f - run * (i + 0.5f) / n, z = kJetFloorZ - rise * (i + 1) / (n + 0.0f);
      stairs.box({x, ym, z - 0.03f}, {run * 0.5f / n + 0.01f, hw, 0.03f}, st, kMatMetal);
    }
    for (float sy : {-1.0f, 1.0f}) {
      const float y = ym + sy * (hw + 0.04f);
      stairs.box({xs - 0.8f - run * 0.5f, y, kJetFloorZ - rise * 0.5f - 0.35f}, {run * 0.5f, 0.02f, rise * 0.5f * 0.3f + 0.2f}, st, kMatMetal);  // stringer
      const Vector3 a{xs - 0.1f, y, kJetFloorZ + 0.95f}, b{xs - 0.8f - run, y, -2.4f + 0.95f};
      const Vector3 m{(a.x + b.x) * 0.5f, y, (a.z + b.z) * 0.5f};
      stairs.box(m, {std::fabs(a.x - b.x) * 0.5f, 0.02f, std::fabs(a.z - b.z) * 0.5f + 0.02f}, rail, kMatMetal);  // (rail, drawn as a slab)
    }
    for (float sy : {-1.0f, 1.0f})  // the unit's chassis beside the stairs
      stairs.box({xs - 0.8f - run * 0.5f, ym + sy * (hw + 0.14f), -2.4f + 0.3f}, {run * 0.5f + 0.4f, 0.08f, 0.3f}, Color{230, 190, 40, 255}, kMatUntinted);
  }
  for (float sg : {1.0f, -1.0f}) {
    const float xi = sg * 1.42f;
    auto W = [&](float ya, float yb, float za, float zb, float xa, float xb) {
      if (sg > 0) cabin.quad({xa, yb, za}, {xa, ya, za}, {xb, ya, zb}, {xb, yb, zb}, wall, kMatUntinted);
      else cabin.quad({-xa, ya, za}, {-xa, yb, za}, {-xb, yb, zb}, {-xb, ya, zb}, wall, kMatUntinted);
    };
    W(y0, y1, -0.72f, 0.12f, 1.35f, 1.42f);  // lower sidewall
    W(y0, y1, 0.58f, 0.85f, 1.42f, 1.3f);    // above the windows
    // window band with openings at the window positions
    float y = y0;
    for (float wy = -10.4f; wy < 11.0f; wy += 0.78f) {
      if (wy - 0.14f < y0 || wy + 0.14f > y1) continue;
      W(y, wy - 0.14f, 0.12f, 0.58f, 1.42f, 1.42f);
      // reveal round the opening
      const float xo = sg * 1.49f;
      auto R = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d) { cabin.quad(a, b, c, d, Color{236, 236, 232, 255}, kMatUntinted); };
      R({xi, wy - 0.14f, 0.16f}, {xi, wy + 0.14f, 0.16f}, {xo, wy + 0.14f, 0.16f}, {xo, wy - 0.14f, 0.16f});
      R({xi, wy + 0.14f, 0.54f}, {xi, wy - 0.14f, 0.54f}, {xo, wy - 0.14f, 0.54f}, {xo, wy + 0.14f, 0.54f});
      W(wy - 0.14f, wy + 0.14f, 0.12f, 0.16f, 1.42f, 1.42f);
      W(wy - 0.14f, wy + 0.14f, 0.54f, 0.58f, 1.42f, 1.42f);
      y = wy + 0.14f;
    }
    W(y, y1, 0.12f, 0.58f, 1.42f, 1.42f);
    cabin.box({sg * 1.08f, (y0 + y1) * 0.5f, 1.0f}, {0.24f, (y1 - y0) * 0.5f, 0.17f}, Color{232, 232, 228, 255}, kMatUntinted);  // bins
  }
  cabin.quad({-0.84f, y1, 1.28f}, {0.84f, y1, 1.28f}, {0.84f, y0, 1.28f}, {-0.84f, y0, 1.28f}, Color{240, 240, 236, 255}, kMatUntinted);
  for (float ry = y0 + 0.6f; ry < y1 - 0.6f; ry += 0.8f)
    for (float x : {-1.0f, -0.55f, 0.55f, 1.0f}) {
      cabin.box({x, ry, -0.33f}, {0.21f, 0.22f, 0.06f}, seat, kMatUntinted);
      cabin.box({x, ry - 0.24f, 0.08f}, {0.21f, 0.05f, 0.38f}, seat, kMatUntinted);
      cabin.box({x, ry - 0.245f, 0.38f}, {0.2f, 0.055f, 0.08f}, head, kMatUntinted);
    }
  m.fuselage = fus.upload();
  m.wings = wings.upload();
  m.gear = gear.upload();
  m.cabin = cabin.upload();
  m.door = door.upload();
  m.stairs = stairs.upload();
  m.nav_red = red.upload();
  m.nav_green = green.upload();
  m.nav_white = white.upload();
  return m;
}

// ------------------------------------------------------------------------------------------------
LightPlaneModel makeLight() {
  LightPlaneModel m;
  Geo fus, rest, prop, pit, marks, needle, yoke;
  // fuselage: rounded-rectangle sections from the tail to the spinner
  struct S {
    float y, w, h, zc;
  };
  const S sec[] = {{-5.8f, 0.08f, 0.3f, 0.28f}, {-4.5f, 0.22f, 0.5f, 0.2f}, {-2.6f, 0.4f, 0.85f, 0.12f}, {-1.3f, 0.55f, 1.3f, 0.07f},
                   {0.0f, 0.56f, 1.42f, 0.06f},  {1.0f, 0.55f, 1.3f, 0.0f},  {1.6f, 0.47f, 0.92f, -0.12f}, {2.15f, 0.36f, 0.7f, -0.12f}};
  const int NS = sizeof(sec) / sizeof(sec[0]);
  auto P = [](const S& s, int k) {
    const float c = 0.3f;  // chamfer fraction
    const float hw = s.w, hh = s.h * 0.5f;
    const Vector3 q[8] = {{hw, s.y, s.zc - hh * (1 - c)}, {hw, s.y, s.zc + hh * (1 - c)}, {hw * (1 - c), s.y, s.zc + hh},     {-hw * (1 - c), s.y, s.zc + hh},
                          {-hw, s.y, s.zc + hh * (1 - c)}, {-hw, s.y, s.zc - hh * (1 - c)}, {-hw * (1 - c), s.y, s.zc - hh}, {hw * (1 - c), s.y, s.zc - hh}};
    return q[k & 7];
  };
  const Color stripe{180, 30, 36, 255};
  for (int i = 0; i + 1 < NS; ++i)
    for (int k = 0; k < 8; ++k) {
      const Color c = (k == 0 || k == 5) && sec[i].y > -5.0f ? stripe : kWhite;
      fus.quad(P(sec[i], k + 1), P(sec[i], k), P(sec[i + 1], k), P(sec[i + 1], k + 1), c, kMatCarPaint);
    }
  // windows: windscreen and side glazing
  fus.quad({0.5f, 1.02f, 0.36f}, {-0.5f, 1.02f, 0.36f}, {-0.42f, 0.42f, 0.78f}, {0.42f, 0.42f, 0.78f}, kWhite, kMatGlass);
  for (float sg : {1.0f, -1.0f}) {
    const float x = sg * 0.566f;
    if (sg > 0) fus.quad({x, -1.1f, 0.12f}, {x, 0.9f, 0.12f}, {x, 0.9f, 0.62f}, {x, -1.1f, 0.62f}, kWhite, kMatGlass);
    else fus.quad({x, 0.9f, 0.12f}, {x, -1.1f, 0.12f}, {x, -1.1f, 0.62f}, {x, 0.9f, 0.62f}, kWhite, kMatGlass);
  }
  // spinner
  for (int k = 0; k < 10; ++k) {
    const float a0 = 2 * PI * k / 10, a1 = 2 * PI * (k + 1) / 10;
    rest.quad({0.2f * std::cos(a1), 2.15f, -0.1f + 0.2f * std::sin(a1)}, {0.2f * std::cos(a0), 2.15f, -0.1f + 0.2f * std::sin(a0)}, {0, 2.55f, -0.1f},
              {0, 2.55f, -0.1f}, kWhite, kMatCarPaint);
  }
  // high wing with red tips, struts
  const Station wr{{0.5f, 0.85f, 0.82f}, 1.5f, 0.2f}, wm{{2.6f, 0.85f, 0.88f}, 1.5f, 0.19f}, wt{{5.5f, 0.72f, 0.97f}, 1.1f, 0.12f};
  for (int side = 0; side < 2; ++side) {
    auto M = [&](Station s) { return side ? mirror(s) : s; };
    surface(rest, M(wr), M(wm), {0, 0, 1}, kWhite, kWhite, false);
    surface(rest, M(wm), M(wt), {0, 0, 1}, kWhite, kWhite);
    const float sx = side ? -1.0f : 1.0f;
    rest.box({sx * 5.45f, 0.2f, 0.97f}, {0.06f, 0.55f, 0.07f}, stripe, kMatUntinted);
    rest.beam({sx * 0.52f, 0.05f, -0.35f}, {sx * 2.6f, 0.2f, 0.74f}, 0.12f, 0.05f, kWhite, kMatCarPaint);
    // main gear leg, wheel, spat
    rest.beam({sx * 0.45f, -0.4f, -0.5f}, {sx * 1.2f, -0.45f, -0.85f}, 0.08f, 0.03f, Color{190, 190, 194, 255}, kMatMetal);
    rest.wheelX({sx * 1.25f, -0.45f, -0.85f}, 0.2f, 0.12f, kDark);
    rest.box({sx * 1.25f, -0.4f, -0.8f}, {0.09f, 0.38f, 0.14f}, kWhite, kMatCarPaint);
  }
  rest.beam({0, 1.35f, -0.4f}, {0, 1.35f, -0.85f}, 0.06f, 0.06f, Color{190, 190, 194, 255}, kMatMetal);
  rest.wheelX({0, 1.35f, -0.85f}, 0.2f, 0.12f, kDark);
  rest.box({0, 1.4f, -0.8f}, {0.08f, 0.34f, 0.13f}, kWhite, kMatCarPaint);
  // tail
  const Station hr{{0.08f, -4.85f, 0.2f}, 0.95f, 0.08f}, ht{{1.75f, -5.0f, 0.2f}, 0.7f, 0.05f};
  surface(rest, hr, ht, {0, 0, 1}, kWhite, kWhite);
  surface(rest, mirror(hr), mirror(ht), {0, 0, 1}, kWhite, kWhite);
  const Station fr{{0, -4.3f, 0.45f}, 1.5f, 0.1f}, ft{{0, -5.2f, 1.65f}, 0.7f, 0.05f};
  surface(rest, fr, ft, {1, 0, 0}, kWhite, kWhite);
  surface(rest, fr, ft, {-1, 0, 0}, kWhite, kWhite);
  rest.box({0, -5.35f, 1.3f}, {0.055f, 0.3f, 0.25f}, stripe, kMatUntinted);
  // propeller (two blades, dark with painted tips) about the y axis at the origin
  for (float sg : {1.0f, -1.0f}) {
    prop.beam({0, 0, sg * 0.1f}, {0, 0, sg * 0.82f}, 0.11f, 0.025f, Color{40, 40, 42, 255}, kMatUntinted, {0, 1, 0});
    prop.beam({0, 0, sg * 0.82f}, {0, 0, sg * 0.95f}, 0.1f, 0.025f, Color{230, 200, 40, 255}, kMatUntinted, {0, 1, 0});
  }
  // cockpit: panel with six instruments, glareshield, frames, seats, yoke
  const Color panel{44, 46, 50, 255}, trim{70, 72, 76, 255}, seatc{90, 78, 64, 255};
  pit.box({0, 1.0f, 0.12f}, {0.56f, 0.05f, 0.27f}, panel, kMatUntinted);
  pit.box({0, 0.93f, 0.41f}, {0.56f, 0.1f, 0.025f}, Color{20, 20, 22, 255}, kMatUntinted);
  const float cols[3] = {-0.47f, -0.3f, -0.13f}, rows[2] = {0.27f, 0.1f};
  int d = 0;
  for (float z : rows)
    for (float x : cols) {
      m.dial[d][0] = x, m.dial[d][1] = 0.945f, m.dial[d][2] = z;
      ++d;
      for (int k = 0; k < 16; ++k) {  // dial face
        const float a0 = 2 * PI * k / 16, a1 = 2 * PI * (k + 1) / 16;
        pit.quad({x + 0.068f * std::sin(a0), 0.948f, z + 0.068f * std::cos(a0)}, {x + 0.068f * std::sin(a1), 0.948f, z + 0.068f * std::cos(a1)}, {x, 0.948f, z},
                 {x, 0.948f, z}, Color{10, 10, 12, 255}, kMatUntinted);
      }
      for (int k = 0; k < 24; ++k) {  // ticks
        const float a = 2 * PI * k / 24;
        const float r0 = k % 2 ? 0.056f : 0.05f;
        const Vector3 dd{std::sin(a), 0, std::cos(a)}, s{std::cos(a), 0, -std::sin(a)};
        const Vector3 b{x, 0.944f, z};
        const Vector3 p0 = Vector3Add(b, Vector3Scale(dd, r0)), p1 = Vector3Add(b, Vector3Scale(dd, 0.064f));
        marks.quad(Vector3Subtract(p0, Vector3Scale(s, 0.0015f)), Vector3Add(p0, Vector3Scale(s, 0.0015f)), Vector3Add(p1, Vector3Scale(s, 0.0015f)),
                   Vector3Subtract(p1, Vector3Scale(s, 0.0015f)), WHITE, kMatSignalLamp);
      }
    }
  // radio stack / engine gauges on the right
  pit.box({0.2f, 0.945f, 0.2f}, {0.12f, 0.004f, 0.12f}, Color{16, 16, 18, 255}, kMatUntinted);
  needle.quad({-0.0022f, -0.002f, -0.008f}, {0.0022f, -0.002f, -0.008f}, {0.0008f, -0.002f, 0.058f}, {-0.0008f, -0.002f, 0.058f}, WHITE, kMatSignalLamp);
  // frames: windscreen posts, door frames, roof under the wing, floor, lower door panels
  for (float sg : {1.0f, -1.0f}) {
    pit.beam({sg * 0.52f, 1.03f, 0.34f}, {sg * 0.44f, 0.42f, 0.8f}, 0.05f, 0.04f, trim, kMatUntinted);
    pit.beam({sg * 0.55f, 0.92f, -0.15f}, {sg * 0.55f, 0.92f, 0.78f}, 0.05f, 0.05f, trim, kMatUntinted);
    pit.beam({sg * 0.55f, -1.15f, -0.15f}, {sg * 0.55f, -1.15f, 0.8f}, 0.06f, 0.05f, trim, kMatUntinted);
    const float x = sg * 0.55f;
    if (sg > 0) pit.quad({x, -1.15f, -0.55f}, {x, 0.95f, -0.55f}, {x, 0.95f, 0.1f}, {x, -1.15f, 0.1f}, Color{120, 110, 98, 255}, kMatUntinted);
    else pit.quad({x, 0.95f, -0.55f}, {x, -1.15f, -0.55f}, {x, -1.15f, 0.1f}, {x, 0.95f, 0.1f}, Color{120, 110, 98, 255}, kMatUntinted);
  }
  pit.beam({0, 1.0f, 0.38f}, {0, 0.44f, 0.8f}, 0.035f, 0.03f, trim, kMatUntinted);
  pit.quad({-0.56f, -1.9f, 0.8f}, {0.56f, -1.9f, 0.8f}, {0.56f, 0.44f, 0.8f}, {-0.56f, 0.44f, 0.8f}, Color{190, 184, 172, 255}, kMatUntinted);
  pit.quad({-0.56f, -1.9f, -0.55f}, {0.56f, -1.9f, -0.55f}, {0.56f, 1.0f, -0.55f}, {-0.56f, 1.0f, -0.55f}, Color{50, 50, 54, 255}, kMatUntinted);
  pit.quad({-0.56f, -1.9f, -0.55f}, {-0.56f, -1.9f, 0.8f}, {0.56f, -1.9f, 0.8f}, {0.56f, -1.9f, -0.55f}, Color{80, 74, 66, 255}, kMatUntinted);
  for (float x : {-0.28f, 0.28f}) {
    pit.box({x, -0.25f, -0.38f}, {0.22f, 0.24f, 0.07f}, seatc, kMatUntinted);
    pit.obox({x, -0.52f, 0.0f}, {0.22f, 0.06f, 0.36f}, {1, 0, 0}, {0, 0.96f, 0.28f}, {0, -0.28f, 0.96f}, seatc, kMatUntinted);
  }
  pit.box({0, -1.4f, -0.36f}, {0.5f, 0.24f, 0.08f}, seatc, kMatUntinted);
  // yoke (centred on its hub): horns and the column stub
  const Color yk{30, 30, 32, 255};
  yoke.beam({-0.17f, 0, 0.0f}, {0.17f, 0, 0.0f}, 0.03f, 0.025f, yk, kMatUntinted, {0, 1, 0});
  for (float sg : {1.0f, -1.0f}) yoke.beam({sg * 0.17f, 0, 0.0f}, {sg * 0.17f, 0, 0.08f}, 0.03f, 0.03f, yk, kMatUntinted, {0, 1, 0});
  yoke.beam({0, 0.0f, 0.0f}, {0, 0.24f, 0.0f}, 0.03f, 0.03f, Color{150, 150, 154, 255}, kMatMetal);
  m.yoke_pos[0] = -0.3f, m.yoke_pos[1] = 0.66f, m.yoke_pos[2] = 0.03f;
  m.fuselage = fus.upload();
  m.rest = rest.upload();
  m.prop = prop.upload();
  m.cockpit = pit.upload();
  m.dial_marks = marks.upload();
  m.needle = needle.upload();
  m.yoke = yoke.upload();
  m.prop_y = 2.35f;
  return m;
}

}  // namespace

void AircraftModels::build() {
  if (ready_) return;
  jet_ = makeJet();
  light_ = makeLight();
  ready_ = true;
}

void AircraftModels::unload() {
  if (!ready_) return;
  for (Mesh* x : {&jet_.fuselage, &jet_.wings, &jet_.gear, &jet_.cabin, &jet_.door, &jet_.stairs, &jet_.nav_red, &jet_.nav_green, &jet_.nav_white, &light_.fuselage, &light_.rest,
                  &light_.prop, &light_.cockpit, &light_.dial_marks, &light_.needle, &light_.yoke})
    if (x->vaoId) {
      UnloadMesh(*x);
      *x = Mesh{};
    }
  ready_ = false;
}

}  // namespace rjc
