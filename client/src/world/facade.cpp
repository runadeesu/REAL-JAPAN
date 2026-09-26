#include "world/facade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "world/coords.hpp"
#include "world/detail.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

using V3 = rj::geo::Vec3d;

uint32_t hashStr(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}
float rnd(uint32_t seed, uint32_t k) {
  uint32_t h = seed ^ (k * 0x9E3779B9u);
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return static_cast<float>(h & 0xffffff) / 16777215.0f;
}

V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }

// Accumulates triangles in raylib space; splits into several meshes below the 16-bit index limit.
class Builder {
 public:
  std::vector<Mesh> meshes;
  size_t total = 0;

  // uv carries (floor, ceiling) heights in raylib space for glass interior mapping.
  float uv_floor = 0.0f, uv_ceil = 0.0f;
  void quad(V3 a, V3 b, V3 c, V3 d, V3 n, Color col, int mat, float param) {
    if (pos_.size() / 3 + 4 > 60000) flush();
    const auto base = static_cast<unsigned short>(pos_.size() / 3);
    for (const V3& p : {a, b, c, d}) {
      const Vector3 r = enuToRl(p);
      pos_.insert(pos_.end(), {r.x, r.y, r.z});
      const Vector3 rn = enuToRl(n);
      nrm_.insert(nrm_.end(), {rn.x, rn.y, rn.z});
      col_.insert(col_.end(), {col.r, col.g, col.b, col.a});
      t2_.insert(t2_.end(), {static_cast<float>(mat), param});
      uv_.insert(uv_.end(), {uv_floor, uv_ceil});
    }
    idx_.insert(idx_.end(), {base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2), base,
                             static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)});
  }
  // Box against a wall: u along the wall, v up, w outward. The back face (-w) is omitted.
  void box(V3 c, V3 u, V3 v, V3 w, double hu, double hv, double hw, Color col, int mat, float param, bool back = false) {
    auto P = [&](double a, double b, double d) { return add(add(add(c, mul(u, a * hu)), mul(v, b * hv)), mul(w, d * hw)); };
    quad(P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), w, col, mat, param);        // front
    quad(P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1), P(-1, 1, -1), v, col, mat, param);        // top
    quad(P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), mul(v, -1), col, mat, param);  // bottom
    quad(P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1), mul(u, -1), col, mat, param);  // left
    quad(P(1, -1, 1), P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), u, col, mat, param);        // right
    if (back) quad(P(1, -1, -1), P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), mul(w, -1), col, mat, param);
  }
  void flush() {
    const int nv = static_cast<int>(pos_.size() / 3);
    if (nv == 0) return;
    if (std::getenv("RJ_FACADE_DUMP")) {  // debug: append geometry as OBJ (raylib space)
      static FILE* f = std::fopen("facades.obj", "w");
      static long vbase = 1;
      if (f) {
        for (int v = 0; v < nv; ++v) std::fprintf(f, "v %.3f %.3f %.3f\n", pos_[v * 3], pos_[v * 3 + 1], pos_[v * 3 + 2]);
        for (size_t t = 0; t + 2 < idx_.size(); t += 3)
          std::fprintf(f, "f %ld %ld %ld\n", vbase + idx_[t], vbase + idx_[t + 1], vbase + idx_[t + 2]);
        vbase += nv;
        std::fflush(f);
      }
    }
    Mesh m{};
    m.vertexCount = nv;
    m.triangleCount = static_cast<int>(idx_.size() / 3);
    m.vertices = pos_.data();
    m.normals = nrm_.data();
    m.colors = col_.data();
    m.texcoords = uv_.data();
    m.texcoords2 = t2_.data();
    m.indices = idx_.data();
    UploadMesh(&m, false);
    m.vertices = m.normals = m.texcoords = m.texcoords2 = nullptr;
    m.colors = nullptr;
    m.indices = nullptr;
    meshes.push_back(m);
    total += static_cast<size_t>(nv);
    pos_.clear();
    nrm_.clear();
    col_.clear();
    t2_.clear();
    uv_.clear();
    idx_.clear();
  }

 private:
  std::vector<float> pos_, nrm_, t2_, uv_;
  std::vector<unsigned char> col_;
  std::vector<unsigned short> idx_;
};

enum class Kind { Shop, Office, Apartment, House, Industrial };

Kind classify(int usage, double height) {
  switch (usage) {
    case 402: case 403: case 404: return Kind::Shop;
    case 401: case 421: case 422: return Kind::Office;
    case 412: case 414: return Kind::Apartment;
    case 411: case 413: case 415: return Kind::House;
    case 431: case 441: return Kind::Industrial;
    default: return height > 12.0 ? Kind::Office : Kind::House;
  }
}

const Color kSignPalette[] = {{236, 236, 230, 255}, {186, 38, 36, 255}, {228, 118, 30, 255}, {38, 128, 70, 255}, {36, 78, 158, 255},
                              {238, 198, 40, 255}, {30, 30, 32, 255}, {116, 78, 48, 255}, {200, 200, 196, 255}};
const Color kAwningPalette[] = {{46, 82, 58, 255}, {120, 34, 38, 255}, {36, 46, 86, 255}, {196, 178, 146, 255}, {60, 60, 62, 255}};

struct Ctx {
  Builder& B;
  uint32_t seed;
  int lod;
  Color frame;
};

// Upper-floor window: glass (+frame, sill at full LOD). o = wall point at window bottom-centre.
void window(Ctx& c, V3 o, V3 u, V3 w, double width, double height, float param, double floor_z, double ceil_z) {
  const V3 up{0, 0, 1};
  const V3 g = add(o, mul(w, 0.018));
  c.B.uv_floor = static_cast<float>(floor_z);
  c.B.uv_ceil = static_cast<float>(ceil_z);
  c.B.quad(add(g, mul(u, -width / 2)), add(g, mul(u, width / 2)), add(add(g, mul(u, width / 2)), mul(up, height)),
           add(add(g, mul(u, -width / 2)), mul(up, height)), w, WHITE, kMatWindow, param);
  if (c.lod > 0) return;
  const double f = 0.05;
  const V3 mid = add(o, mul(up, height / 2));
  c.B.box(add(add(mid, mul(u, -width / 2 - f / 2)), mul(w, 0.03)), u, up, w, f / 2, height / 2 + f, 0.03, c.frame, kMatFrame, 0);
  c.B.box(add(add(mid, mul(u, width / 2 + f / 2)), mul(w, 0.03)), u, up, w, f / 2, height / 2 + f, 0.03, c.frame, kMatFrame, 0);
  c.B.box(add(add(o, mul(up, height + f / 2)), mul(w, 0.03)), u, up, w, width / 2, f / 2, 0.03, c.frame, kMatFrame, 0);
  c.B.box(add(add(o, mul(up, -0.03)), mul(w, 0.06)), u, up, w, width / 2 + 0.06, 0.03, 0.06, Color{190, 188, 182, 255}, kMatConcrete, 0);
  if (width > 1.3) c.B.box(add(mid, mul(w, 0.03)), u, up, w, 0.025, height / 2, 0.025, c.frame, kMatFrame, 0);  // mullion
}

void shopFront(Ctx& c, V3 a, V3 u, V3 w, double L, double gh, std::vector<V3>& lights) {
  const V3 up{0, 0, 1};
  const double z_glass0 = 0.12, z_glass1 = std::min(gh - 0.9, 2.9), z_sign = std::min(gh - 0.45, 3.4);
  auto at = [&](double x, double z, double o) { return add(add(add(a, mul(u, x)), mul(up, z)), mul(w, o)); };
  // Split long frontages into tenant units (each with its own sign band, door and awning).
  const int units = std::max(1, static_cast<int>(std::lround(L / 7.5)));
  const double uw = L / units;
  for (int k = 0; k < units; ++k) {
    const uint32_t us = c.seed ^ (0x51ED27u * static_cast<uint32_t>(k + 1));
    const double x0 = k * uw + 0.3, x1 = (k + 1) * uw - 0.3;
    if (x1 - x0 < 1.4) continue;
    const float lit = 2.0f + 0.98f * rnd(us, 900);
    c.B.uv_floor = static_cast<float>(a.z + 0.02);
    c.B.uv_ceil = static_cast<float>(a.z + z_glass1 + 0.35);
    c.B.quad(at(x0, z_glass0, 0.03), at(x1, z_glass0, 0.03), at(x1, z_glass1, 0.03), at(x0, z_glass1, 0.03), w, WHITE, kMatShopGlass, lit);
    c.B.box(at((x0 + x1) / 2, z_glass0 / 2, 0.05), u, up, w, (x1 - x0) / 2, z_glass0 / 2, 0.05, Color{120, 118, 114, 255}, kMatConcrete, 0);
    // Pillar between units (wall-coloured cladding).
    c.B.box(at(k * uw, (z_sign + 0.4) / 2, 0.06), u, up, w, 0.3, (z_sign + 0.4) / 2, 0.06, Color{150, 146, 140, 255}, kMatConcrete, 0);
    // Sign band: blank (shop names are not in the data), colour per tenant.
    const Color sc = kSignPalette[static_cast<int>(rnd(us, 901) * 8.99f)];
    c.B.box(at((x0 + x1) / 2, z_sign, 0.1), u, up, w, (x1 - x0) / 2 + 0.25, 0.36, 0.1, sc, kMatSignBand, lit);
    if (c.lod == 0) {
      const int nm = std::max(1, static_cast<int>((x1 - x0) / 1.6));
      for (int i = 0; i <= nm; ++i) {
        const double x = x0 + (x1 - x0) * i / nm;
        c.B.box(at(x, (z_glass0 + z_glass1) / 2, 0.05), u, up, w, 0.03, (z_glass1 - z_glass0) / 2, 0.04, c.frame, kMatFrame, 0);
      }
      c.B.box(at((x0 + x1) / 2, z_glass1 + 0.02, 0.05), u, up, w, (x1 - x0) / 2, 0.04, 0.05, c.frame, kMatFrame, 0);
      const double dx = x0 + 0.9 + std::max(0.0, x1 - x0 - 1.8) * rnd(us, 902);
      c.B.quad(at(dx - 0.8, z_glass0, 0.045), at(dx + 0.8, z_glass0, 0.045), at(dx + 0.8, 2.1, 0.045), at(dx - 0.8, 2.1, 0.045), w,
               Color{40, 40, 44, 255}, kMatShopGlass, lit);  // automatic door
      c.B.box(at(dx, 1.05, 0.06), u, up, w, 0.02, 1.05, 0.03, c.frame, kMatFrame, 0);
      c.B.box(at(dx, 2.12, 0.06), u, up, w, 0.82, 0.04, 0.04, c.frame, kMatFrame, 0);
    }
    if (rnd(us, 903) < 0.4f) {  // awning (庇)
      const Color ac = kAwningPalette[static_cast<int>(rnd(us, 904) * 4.99f)];
      const double z = z_glass1 + 0.1, depth = 0.9;
      c.B.quad(at(x0, z, 0.0), at(x1, z, 0.0), at(x1, z - 0.32, depth), at(x0, z - 0.32, depth), V3{w.x * 0.34, w.y * 0.34, 0.94}, ac,
               kMatAwning, 0);
      c.B.quad(at(x0, z - 0.32, depth), at(x1, z - 0.32, depth), at(x1, z - 0.55, depth), at(x0, z - 0.55, depth), w, ac, kMatAwning, 0);
    }
    lights.push_back(at((x0 + x1) / 2, 2.4, 1.3));
  }
  c.B.uv_floor = c.B.uv_ceil = 0.0f;
}

void generate(const World& world, const LoadedCell& cell, int bi, int lod, Builder& B, std::vector<V3>& lights) {
  const BuildingInfo& info = cell.cpu->buildings[static_cast<size_t>(bi)];
  const auto& fp = cell.fp[static_cast<size_t>(bi)];
  if (fp.size() < 3) return;
  const double z0 = cell.zmin[static_cast<size_t>(bi)], ztop = cell.zmax[static_cast<size_t>(bi)];
  const double H = ztop - z0;
  if (H < 2.5) return;
  double area2 = 0;
  for (size_t k = 0; k < fp.size(); ++k) {
    const auto& p = fp[k];
    const auto& q = fp[(k + 1) % fp.size()];
    area2 += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
  }
  if (std::fabs(area2) < 24.0) return;  // < 12 m2: sheds, kiosks
  const double orient = area2 > 0 ? 1.0 : -1.0;
  const uint32_t seed = hashStr(info.id);
  const Kind kind = classify(info.usage, H);
  int storeys = (info.storeys_above >= 1 && info.storeys_above < 200) ? info.storeys_above
                                                                      : std::max(1, static_cast<int>(std::lround(H / 3.3)));
  const double roof_allow = (info.lod >= 2 && (kind == Kind::House)) ? 2.0 : 0.6;
  double sh = std::clamp((H - roof_allow) / storeys, 2.6, 5.5);
  storeys = std::max(1, std::min(storeys, static_cast<int>((H - 0.3) / sh)));
  const double gh = (kind == Kind::Shop || kind == Kind::Office) ? std::clamp(sh * 1.1, 3.4, 4.4) : sh;
  Ctx c{B, seed, lod, rnd(seed, 1) < 0.6f ? Color{176, 178, 182, 255} : Color{74, 68, 62, 255}};
  const V3 up{0, 0, 1};
  // Street-facing edges (the outside 2 m in front of the wall is not another building).
  struct Edge { V3 a, u, w; double L; bool street; };
  std::vector<Edge> edges;
  size_t longest = 0;
  for (size_t k = 0; k < fp.size(); ++k) {
    const auto& p = fp[k];
    const auto& q = fp[(k + 1) % fp.size()];
    const double dx = q.x - p.x, dy = q.y - p.y, L = std::hypot(dx, dy);
    if (L < 2.0) continue;
    const V3 u{dx / L, dy / L, 0};
    const V3 w{u.y * orient, -u.x * orient, 0};
    const double mx = p.x + dx / 2 + w.x * 2.0, my = p.y + dy / 2 + w.y * 2.0;
    const bool street = !world.pointInBuilding(mx, my) && world.terrainHeight(mx, my).has_value();
    edges.push_back({V3{p.x, p.y, z0}, u, w, L, street});
    if (street && (edges.size() == 1 || L > edges[longest].L || !edges[longest].street)) longest = edges.size() - 1;
  }
  const bool balconies = kind == Kind::Apartment && storeys >= 2;
  bool door_done = false;
  for (size_t ei = 0; ei < edges.size(); ++ei) {
    const Edge& e = edges[ei];
    const V3 a = e.a, u = e.u, w = e.w;
    auto at = [&](double x, double z) { return add(add(a, mul(u, x)), mul(up, z)); };
    // --- ground floor -----------------------------------------------------
    if (e.street) {
      if (kind == Kind::Shop || (kind == Kind::Office && info.usage == 0 && e.L > 5.0) || (kind == Kind::Apartment && info.usage == 414)) {
        shopFront(c, a, u, w, e.L, gh, lights);
      } else if (kind == Kind::Office && ei == longest) {
        const double gw = std::min(e.L * 0.6, 12.0), x0 = (e.L - gw) / 2;
        const float lit = 0.0f + 0.98f * rnd(seed, 950);
        c.B.uv_floor = static_cast<float>(a.z + 0.05);
        c.B.uv_ceil = static_cast<float>(a.z + 3.4);
        c.B.quad(add(at(x0, 0.1), mul(w, 0.03)), add(at(x0 + gw, 0.1), mul(w, 0.03)), add(at(x0 + gw, 2.9), mul(w, 0.03)),
                 add(at(x0, 2.9), mul(w, 0.03)), w, WHITE, kMatShopGlass, lit);
        if (lod == 0)
          for (int i = 0; i <= static_cast<int>(gw / 1.5); ++i)
            c.B.box(add(at(x0 + i * gw / std::max(1, static_cast<int>(gw / 1.5)), 1.5), mul(w, 0.05)), u, up, w, 0.03, 1.4, 0.04, c.frame,
                    kMatFrame, 0);
      } else if (!door_done && (kind == Kind::House || kind == Kind::Apartment) && e.L > 3.0) {
        const double dx = 0.8 + (e.L - 1.6) * rnd(seed, 960);
        c.B.box(add(at(dx, 1.0), mul(w, 0.03)), u, up, w, 0.45, 1.0, 0.03, Color{96, 74, 54, 255}, kMatWallPaint, 0);
        c.B.box(add(at(dx, 2.35), mul(w, 0.45)), u, up, w, 0.7, 0.04, 0.45, Color{160, 158, 154, 255}, kMatConcrete, 0);  // canopy
        door_done = true;
      }
    }
    // --- upper floors -----------------------------------------------------
    if (info.textured) continue;  // real facade imagery: never paint invented windows over it
    const bool balc = balconies && e.street && (ei == longest) && e.L >= 5.0;
    const int k0 = (kind == Kind::House || kind == Kind::Industrial) ? 0 : 1;
    for (int k = k0; k < storeys; ++k) {
      const double fz = (k == 0 ? 0.0 : gh + (k - 1) * sh);
      const double hh = k == 0 ? gh : sh;
      if (fz + hh > H + 0.01) break;
      if (balc && k >= 1) {
        // Balcony: slab + parapet, sliding doors and an AC unit per bay.
        const double L = e.L - 0.3;
        c.B.box(add(at(e.L / 2, fz), mul(w, 0.55)), u, up, w, L / 2, 0.08, 0.55, Color{198, 196, 190, 255}, kMatConcrete, 0);
        c.B.box(add(at(e.L / 2, fz + 0.55), mul(w, 1.06)), u, up, w, L / 2, 0.47, 0.05, Color{206, 204, 198, 255}, kMatConcrete, 0, true);
        const int nb = std::max(1, static_cast<int>(e.L / 3.2));
        for (int b = 0; b < nb; ++b) {
          const double x = e.L * (b + 0.5) / nb;
          const float lit = 1.0f + 0.98f * rnd(seed, 1000 + static_cast<uint32_t>(k * 64 + b));
          const V3 o = add(at(x, fz + 0.1), mul(w, 0.01));
          c.B.uv_floor = static_cast<float>(a.z + fz + 0.05);
          c.B.uv_ceil = static_cast<float>(a.z + fz + sh - 0.05);
          c.B.quad(add(o, mul(u, -0.85)), add(o, mul(u, 0.85)), add(add(o, mul(u, 0.85)), mul(up, 2.0)), add(add(o, mul(u, -0.85)), mul(up, 2.0)),
                   w, WHITE, kMatWindow, lit);
          if (lod == 0 && rnd(seed, 2000 + static_cast<uint32_t>(k * 64 + b)) < 0.85f)
            c.B.box(add(at(x + (rnd(seed, 3000 + b) < 0.5f ? -1.2 : 1.2), fz + 0.39), mul(w, 0.5)), u, up, w, 0.39, 0.31, 0.14,
                    Color{226, 226, 220, 255}, kMatAcUnit, 0);
        }
        continue;
      }
      if (k == 0 && e.street && (kind == Kind::Shop)) continue;
      double bay, ww, wh, wb;
      float cat;
      switch (kind) {
        case Kind::Office: case Kind::Shop: bay = 1.8; ww = 1.45; wh = 0.62 * hh; wb = 0.2 * hh; cat = 0.0f; break;
        case Kind::Industrial: bay = 3.6; ww = 1.8; wh = 0.35 * hh; wb = 0.5 * hh; cat = 0.0f; break;
        case Kind::Apartment: bay = 2.6; ww = 1.1; wh = 0.42 * hh; wb = 0.34 * hh; cat = 1.0f; break;
        default: bay = 2.9; ww = 1.2; wh = 0.4 * hh; wb = 0.33 * hh; cat = 1.0f; break;
      }
      if (!e.street) ww *= 0.8;
      const int n = static_cast<int>((e.L - 0.8) / bay);
      if (n < 1) continue;
      const double start = (e.L - n * bay) / 2 + bay / 2;
      for (int i = 0; i < n; ++i) {
        if (kind == Kind::House && rnd(seed, 4000 + static_cast<uint32_t>(k * 97 + i + ei * 13)) < 0.3f) continue;
        const float lit = cat + 0.98f * rnd(seed, 5000 + static_cast<uint32_t>(k * 131 + i + ei * 17));
        window(c, at(start + i * bay, fz + wb), u, w, ww, wh, lit, a.z + fz + 0.02, a.z + fz + hh - 0.05);
      }
    }
  }
  // Drain pipe down one street corner (full LOD only).
  if (lod == 0 && !edges.empty() && edges[longest].street) {
    const Edge& e = edges[longest];
    const double h = std::min(H, gh + (storeys - 1) * sh);
    c.B.box(add(add(e.a, mul(e.u, 0.18)), add(mul(e.w, 0.07), mul(up, h / 2))), e.u, up, e.w, 0.045, h / 2, 0.045,
            Color{150, 150, 146, 255}, kMatMetal, 0);
  }
}

}  // namespace

void FacadeDetail::release(Entry& e) {
  for (auto& m : e.meshes) {
    verts_ -= static_cast<size_t>(m.vertexCount);
    UnloadMesh(m);
  }
  e.meshes.clear();
  e.lights.clear();
}

void FacadeDetail::clear() {
  for (auto& [k, e] : map_) release(e);
  map_.clear();
  verts_ = 0;
}

void FacadeDetail::update(const World& world, const V3& player, int budget) {
  constexpr double kNear = 60.0, kFar = 160.0, kEvict = 185.0;
  for (auto& [k, e] : map_) e.alive = false;
  struct Job {
    double d;
    const LoadedCell* cell;
    int b;
    int lod;
    std::string key;
  };
  std::vector<Job> jobs;
  for (const auto& [code, cell] : world.cells()) {
    if (!cell->cpu) continue;
    for (size_t b = 0; b < cell->fp.size(); ++b) {
      const auto& poly = cell->fp[b];
      if (poly.empty()) continue;
      double d = 1e30;
      for (const auto& p : poly) d = std::min(d, std::hypot(p.x - player.x, p.y - player.y));
      if (d > kEvict) continue;
      std::string key = code + ":" + std::to_string(b);
      const int want = d < kNear ? 0 : (d < kFar ? 1 : -1);
      auto it = map_.find(key);
      if (it != map_.end()) {
        it->second.alive = true;
        // hysteresis: keep the current LOD within +10 m of its boundary
        const int cur = it->second.lod;
        const bool keep = (cur == 0 && d < kNear + 10) || (cur == 1 && d > kNear - 10 && d < kFar + 10) || (want == -1 && d < kEvict);
        if (keep || cur == want) continue;
      }
      if (want < 0) continue;
      jobs.push_back({d, cell.get(), static_cast<int>(b), want, std::move(key)});
    }
  }
  for (auto it = map_.begin(); it != map_.end();) {
    if (!it->second.alive) {
      release(it->second);
      it = map_.erase(it);
    } else {
      ++it;
    }
  }
  std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.d < b.d; });
  int done = 0;
  for (const auto& j : jobs) {
    if (done >= budget) break;
    Entry& e = map_[j.key];
    release(e);
    Builder B;
    generate(world, *j.cell, j.b, j.lod, B, e.lights);
    B.flush();
    e.meshes = std::move(B.meshes);
    verts_ += B.total;
    e.lod = j.lod;
    e.alive = true;
    ++done;
  }
}

void FacadeDetail::collectLights(const V3& cam, double max_d, std::vector<V3>& out) const {
  for (const auto& [k, e] : map_)
    for (const auto& l : e.lights)
      if (std::hypot(l.x - cam.x, l.y - cam.y) < max_d) out.push_back(l);
}

}  // namespace rjc
