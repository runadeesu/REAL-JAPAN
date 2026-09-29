#include "render/ships.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/deck_layout.hpp"
#include "game/ferries.hpp"
#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned int> idx;
  unsigned int v() const { return static_cast<unsigned int>(pos.size() / 3); }
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
    n = Vector3Normalize(n);
    const unsigned int k = v();
    vert(a, n, col, mat);
    vert(b, n, col, mat);
    vert(c, n, col, mat);
    vert(d, n, col, mat);
    idx.insert(idx.end(), {k, k + 1, k + 2, k, k + 2, k + 3});
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
    // raylib meshes index with 16 bits: split into chunks is not needed below 65k vertices
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    if (!m.vertexCount) return m;
    std::vector<unsigned short> i16(idx.begin(), idx.end());
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.texcoords2 = t2.data();
    m.colors = col.data();
    m.indices = i16.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    return m;
  }
};

const Color kWhite{236, 237, 239, 255}, kRed{128, 44, 38, 255}, kBlack{24, 24, 26, 255}, kBlue{28, 78, 158, 255};
const Color kDeck{96, 112, 102, 255}, kRail{215, 216, 218, 255}, kOrange{226, 110, 30, 255};

struct HullSec {
  float y, w_deck, w_wl, z_keel;
};

// A deckhouse block with a window band on its sides and front.
void house(Geo& g, float x, float y0, float y1, float z0, float h, bool front_windows, bool back_windows = false) {
  g.box({0, (y0 + y1) * 0.5f, z0 + h * 0.5f}, {x, (y1 - y0) * 0.5f, h * 0.5f}, kWhite, kMatUntinted);
  const float wz0 = z0 + h * 0.36f, wz1 = z0 + h * 0.78f, e = 0.02f;
  for (float sg : {1.0f, -1.0f}) {
    const float xx = sg * (x + e);
    if (sg > 0) g.quad({xx, y0 + 0.8f, wz0}, {xx, y1 - 0.8f, wz0}, {xx, y1 - 0.8f, wz1}, {xx, y0 + 0.8f, wz1}, kWhite, kMatGlass);
    else g.quad({xx, y1 - 0.8f, wz0}, {xx, y0 + 0.8f, wz0}, {xx, y0 + 0.8f, wz1}, {xx, y1 - 0.8f, wz1}, kWhite, kMatGlass);
    for (float y = y0 + 3.0f; y < y1 - 1.0f; y += 3.0f)  // mullions
      g.box({sg * (x + 0.04f), y, (wz0 + wz1) * 0.5f}, {0.03f, 0.08f, (wz1 - wz0) * 0.5f}, kWhite, kMatUntinted);
  }
  if (front_windows) g.quad({x - 0.6f, y1 + e, wz0}, {-(x - 0.6f), y1 + e, wz0}, {-(x - 0.6f), y1 + e, wz1}, {x - 0.6f, y1 + e, wz1}, kWhite, kMatGlass);
  if (back_windows) g.quad({-(x - 0.6f), y0 - e, wz0}, {x - 0.6f, y0 - e, wz0}, {x - 0.6f, y0 - e, wz1}, {-(x - 0.6f), y0 - e, wz1}, kWhite, kMatGlass);
}

void railing(Geo& g, Vector3 a, Vector3 b) {
  const float len = std::hypot(b.x - a.x, b.y - a.y);
  const int n = std::max(1, static_cast<int>(len / 2.2f));
  const Vector3 d{(b.x - a.x) / len, (b.y - a.y) / len, 0};
  const bool along_y = std::fabs(d.y) > std::fabs(d.x);
  const Vector3 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, a.z};
  const Vector3 h = along_y ? Vector3{0.03f, len * 0.5f, 0.03f} : Vector3{len * 0.5f, 0.03f, 0.03f};
  g.box({mid.x, mid.y, a.z + 1.05f}, h, kRail, kMatMetal);
  g.box({mid.x, mid.y, a.z + 0.55f}, {h.x * 0.7f + (along_y ? 0 : 0), h.y, 0.015f}, kRail, kMatMetal);
  for (int i = 0; i <= n; ++i) {
    const float t = static_cast<float>(i) / n;
    g.box({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + 0.53f}, {0.025f, 0.025f, 0.53f}, kRail, kMatMetal);
  }
}

ShipModel makeShip(int cls) {
  const ShipClass& C = Ferries::shipClass(cls);
  Geo g;
  const float L = C.length, B = C.beam * 0.5f, fb = C.freeboard;
  const float draft = cls == 0 ? 2.8f : 5.0f;
  const float y0 = -L * 0.5f, y1 = L * 0.5f;
  // hull sections: transom stern, parallel mid body, fine raked bow
  std::vector<HullSec> s;
  const int N = 26;
  for (int i = 0; i <= N; ++i) {
    const float t = static_cast<float>(i) / N;
    const float y = y0 + t * L;
    float wd = B, ww = B * 0.96f;
    if (t < 0.12f) {  // stern: slight narrowing
      const float u = t / 0.12f;
      wd = B * (0.9f + 0.1f * u);
      ww = B * (0.8f + 0.16f * u);
    }
    if (t > 0.62f) {  // bow entrance
      const float u = (t - 0.62f) / 0.38f;
      wd = B * std::max(0.02f, std::cos(u * u * 1.45f) * (1.0f - 0.12f * u));
      ww = B * 0.96f * std::max(0.0f, 1.0f - std::pow(u, 1.25f)) * (u > 0.93f ? 0.0f : 1.0f);
    }
    const float zk = -draft + (t > 0.85f ? (t - 0.85f) / 0.15f * draft * 0.8f : 0.0f) + (t < 0.05f ? (0.05f - t) * 20.0f : 0.0f);
    s.push_back({y, wd, ww, zk});
  }
  const float zboot0 = -0.25f, zboot1 = 0.45f;
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    const HullSec& a = s[i];
    const HullSec& b = s[i + 1];
    for (float sg : {1.0f, -1.0f}) {
      auto Q = [&](Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, Color c, int mat) {
        if (sg > 0) g.quad(p0, p1, p2, p3, c, mat);
        else g.quad({-p1.x, p1.y, p1.z}, {-p0.x, p0.y, p0.z}, {-p3.x, p3.y, p3.z}, {-p2.x, p2.y, p2.z}, c, mat);
      };
      const float ab = a.w_wl * 0.62f, bb = b.w_wl * 0.62f;
      Q({ab, a.y, a.z_keel}, {bb, b.y, b.z_keel}, {b.w_wl, b.y, zboot0}, {a.w_wl, a.y, zboot0}, kRed, kMatUntinted);    // bottom
      auto W = [&](const HullSec& h, float z) { return h.w_wl + (h.w_deck - h.w_wl) * std::clamp((z - zboot0) / (fb - zboot0), 0.0f, 1.0f); };
      Q({a.w_wl, a.y, zboot0}, {b.w_wl, b.y, zboot0}, {W(b, zboot1), b.y, zboot1}, {W(a, zboot1), a.y, zboot1}, kBlack, kMatUntinted);  // boot-top
      const float zs0 = fb - 1.3f, zs1 = fb - 0.75f;
      Q({W(a, zboot1), a.y, zboot1}, {W(b, zboot1), b.y, zboot1}, {W(b, zs0), b.y, zs0}, {W(a, zs0), a.y, zs0}, kWhite, kMatUntinted);
      Q({W(a, zs0), a.y, zs0}, {W(b, zs0), b.y, zs0}, {W(b, zs1), b.y, zs1}, {W(a, zs1), a.y, zs1}, kBlue, kMatUntinted);  // livery stripe
      Q({W(a, zs1), a.y, zs1}, {W(b, zs1), b.y, zs1}, {b.w_deck, b.y, fb}, {a.w_deck, a.y, fb}, kWhite, kMatUntinted);
      Q({a.w_deck, a.y, fb}, {b.w_deck, b.y, fb}, {b.w_deck, b.y, fb + 1.0f}, {a.w_deck, a.y, fb + 1.0f}, kWhite, kMatUntinted);  // bulwark
    }
    g.quad({a.w_deck, a.y, fb}, {b.w_deck, b.y, fb}, {-b.w_deck, b.y, fb}, {-a.w_deck, a.y, fb}, kDeck, kMatUntinted);  // main deck
  }
  // transom
  const HullSec& t0 = s.front();
  g.quad({-t0.w_deck, y0, fb + 1.0f}, {t0.w_deck, y0, fb + 1.0f}, {t0.w_wl, y0, zboot0}, {-t0.w_wl, y0, zboot0}, kWhite, kMatUntinted);
  g.quad({-t0.w_wl, y0, zboot0}, {t0.w_wl, y0, zboot0}, {t0.w_wl * 0.62f, y0, t0.z_keel}, {-t0.w_wl * 0.62f, y0, t0.z_keel}, kRed, kMatUntinted);
  if (cls == 1) {  // stern car ramp (raised)
    g.quad({-B * 0.45f, y0 - 0.06f, fb + 0.2f}, {B * 0.45f, y0 - 0.06f, fb + 0.2f}, {B * 0.45f, y0 - 0.06f, fb + 5.0f}, {-B * 0.45f, y0 - 0.06f, fb + 5.0f},
           Color{70, 72, 76, 255}, kMatUntinted);
  }
  // deckhouses: lower deck(s) up to the open passenger deck, then the upper cabin and bridge
  const int lower_levels = cls == 0 ? 1 : 2;
  const float lvl = (C.deck_z - fb) / lower_levels;
  for (int k = 0; k < lower_levels; ++k)
    house(g, B - 0.9f, C.deck_y0, C.deck_y1, fb + lvl * k, lvl, true, true);
  g.box({0, (C.deck_y0 + C.deck_y1) * 0.5f, C.deck_z - 0.05f}, {B - 0.85f, (C.deck_y1 - C.deck_y0) * 0.5f + 0.05f, 0.06f}, kDeck, kMatUntinted);
  const float hz = C.deck_z, hh = 2.8f;
  house(g, C.house_x, C.house_y0, C.house_y1, hz, hh, false, true);
  // bridge on top, with a raked window band and wings
  const float by0 = C.house_y1 - (cls == 0 ? 6.0f : 9.0f), by1 = C.house_y1 + 0.6f, bz = hz + hh;
  g.box({0, (by0 + by1) * 0.5f, bz + 1.3f}, {C.house_x, (by1 - by0) * 0.5f, 1.3f}, kWhite, kMatUntinted);
  g.quad({C.house_x, by1 + 0.02f, bz + 1.1f}, {-C.house_x, by1 + 0.02f, bz + 1.1f}, {-C.house_x, by1 - 0.4f, bz + 2.3f}, {C.house_x, by1 - 0.4f, bz + 2.3f},
         kWhite, kMatGlass);
  for (float sg : {1.0f, -1.0f}) {
    g.box({sg * (C.house_x + (B - C.house_x) * 0.5f), by1 - 1.2f, bz + 1.2f}, {(B - C.house_x) * 0.5f, 1.1f, 1.2f}, kWhite, kMatUntinted);
    g.quad({sg * (C.house_x + 0.02f), by0 + 0.8f, bz + 1.1f}, {sg * (C.house_x + 0.02f), by1 - 2.6f, bz + 1.1f}, {sg * (C.house_x + 0.02f), by1 - 2.6f, bz + 2.2f},
           {sg * (C.house_x + 0.02f), by0 + 0.8f, bz + 2.2f}, kWhite, kMatGlass);
  }
  g.box({0, (by0 + by1) * 0.5f, bz + 2.65f}, {C.house_x + 0.3f, (by1 - by0) * 0.5f + 0.3f, 0.06f}, kWhite, kMatUntinted);
  // mast with radar
  g.box({0, by0 + 2.0f, bz + 2.7f + 2.5f}, {0.12f, 0.12f, 2.5f}, kRail, kMatMetal);
  g.box({0, by0 + 2.0f, bz + 2.7f + 3.2f}, {1.4f, 0.12f, 0.08f}, kBlack, kMatUntinted);
  g.box({0, by0 + 2.0f, bz + 2.7f + 4.6f}, {0.8f, 0.1f, 0.05f}, kRail, kMatMetal);
  // funnel (generic colours)
  const float fy = C.house_y0 + (cls == 0 ? 3.0f : 5.0f), fz = hz + hh;
  const float fw = cls == 0 ? 1.4f : 2.4f, fl = cls == 0 ? 2.2f : 3.6f, fh = cls == 0 ? 4.2f : 6.5f;
  g.box({0, fy, fz + fh * 0.5f}, {fw, fl, fh * 0.5f}, kWhite, kMatUntinted);
  g.box({0, fy, fz + fh * 0.62f}, {fw + 0.02f, fl + 0.02f, fh * 0.12f}, kBlue, kMatUntinted);
  g.box({0, fy, fz + fh - 0.3f}, {fw + 0.02f, fl + 0.02f, 0.3f}, kBlack, kMatUntinted);
  // railings round the open deck and the main deck, life-raft canisters
  const float dz = C.deck_z;
  railing(g, {-C.deck_x - 0.4f, C.deck_y0, dz}, {-C.deck_x - 0.4f, C.deck_y1, dz});
  railing(g, {C.deck_x + 0.4f, C.deck_y0, dz}, {C.deck_x + 0.4f, C.deck_y1, dz});
  railing(g, {-C.deck_x - 0.4f, C.deck_y0, dz}, {C.deck_x + 0.4f, C.deck_y0, dz});
  railing(g, {-C.deck_x - 0.4f, C.deck_y1, dz}, {C.deck_x + 0.4f, C.deck_y1, dz});
  for (float sg : {1.0f, -1.0f})
    for (float y = C.house_y0 + 2.0f; y < C.house_y1 - 3.0f; y += cls == 0 ? 4.0f : 5.0f) {
      g.box({sg * (C.house_x + 0.7f), y, dz + 0.45f}, {0.35f, 0.7f, 0.35f}, Color{230, 230, 226, 255}, kMatUntinted);
      g.box({sg * (C.house_x + 0.7f), y, dz + 0.45f}, {0.36f, 0.12f, 0.36f}, kOrange, kMatUntinted);
    }
  // benches in rows on the open deck aft of the deckhouse, facing aft (game/deck_layout.hpp)
  const Color bench{150, 110, 70, 255}, frame{90, 92, 96, 255};
  for (const auto& r : ferryBenchRows(C))
    for (float sg : {1.0f, -1.0f}) {
      const float xc = sg * (r.x0 + r.x1 + 0.25f) * 0.5f, hx = (r.x1 + 0.25f - r.x0) * 0.5f;
      g.box({xc, r.y + 0.02f, dz + 0.43f}, {hx, 0.22f, 0.035f}, bench, kMatUntinted);   // seat slats
      g.box({xc, r.y + 0.27f, dz + 0.78f}, {hx, 0.03f, 0.2f}, bench, kMatUntinted);    // backrest
      for (float lx : {-hx + 0.08f, 0.0f, hx - 0.08f})                                 // frames
        g.box({xc + lx, r.y + 0.1f, dz + 0.3f}, {0.03f, 0.2f, 0.3f}, frame, kMatUntinted);
    }
  // foredeck: bollards and a windlass
  for (float sg : {1.0f, -1.0f}) g.box({sg * B * 0.45f, y1 - L * 0.16f, fb + 0.3f}, {0.25f, 0.25f, 0.3f}, kBlack, kMatUntinted);
  g.box({0, y1 - L * 0.12f, fb + 0.5f}, {0.8f, 0.6f, 0.5f}, Color{60, 90, 70, 255}, kMatUntinted);
  ShipModel m;
  m.hull = g.upload();
  m.length = L;
  return m;
}

}  // namespace

void ShipModels::build() {
  if (ready_) return;
  m_[0] = makeShip(0);
  m_[1] = makeShip(1);
  ready_ = true;
}

void ShipModels::unload() {
  if (!ready_) return;
  for (auto& m : m_)
    if (m.hull.vaoId) {
      UnloadMesh(m.hull);
      m.hull = Mesh{};
    }
  ready_ = false;
}

}  // namespace rjc
