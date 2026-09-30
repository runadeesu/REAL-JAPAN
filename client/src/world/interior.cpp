#include "world/interior.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "platform/paths.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

int64_t key(int bx, int by) { return (static_cast<int64_t>(bx) << 32) ^ static_cast<uint32_t>(by); }

class Reader {
 public:
  explicit Reader(const std::vector<unsigned char>& d) : d_(d) {}
  bool ok() const { return ok_; }
  template <class T>
  T get() {
    T v{};
    bytes(&v, sizeof(T));
    return v;
  }
  void bytes(void* dst, size_t n) {
    if (p_ + n > d_.size()) {
      ok_ = false;
      return;
    }
    if (n) std::memcpy(dst, d_.data() + p_, n);
    p_ += n;
  }
  void skip(size_t n) {
    if (p_ + n > d_.size()) ok_ = false;
    else p_ += n;
  }
  std::string str() {
    const uint32_t n = get<uint32_t>();
    if (!ok_ || p_ + n > d_.size()) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(d_.data() + p_), n);
    p_ += n;
    return s;
  }

 private:
  const std::vector<unsigned char>& d_;
  size_t p_ = 0;
  bool ok_ = true;
};

// 2D point-in-triangle; returns barycentric-interpolated z.
std::optional<double> zAt(const float* a, const float* b, const float* c, double x, double y) {
  const double d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
  if (std::fabs(d) < 1e-9) return std::nullopt;
  const double l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
  const double l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
  const double l3 = 1.0 - l1 - l2;
  if (l1 < -1e-4 || l2 < -1e-4 || l3 < -1e-4) return std::nullopt;
  return l1 * a[2] + l2 * b[2] + l3 * c[2];
}

// Moller-Trumbore ray/triangle intersection; returns the ray parameter.
std::optional<double> rayTri(const rj::geo::Vec3d& o, const rj::geo::Vec3d& d, const float* a, const float* b, const float* c) {
  const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const double e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const double p[3] = {d.y * e2[2] - d.z * e2[1], d.z * e2[0] - d.x * e2[2], d.x * e2[1] - d.y * e2[0]};
  const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-12) return std::nullopt;
  const double inv = 1.0 / det;
  const double t[3] = {o.x - a[0], o.y - a[1], o.z - a[2]};
  const double u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * inv;
  if (u < 0.0 || u > 1.0) return std::nullopt;
  const double q[3] = {t[1] * e1[2] - t[2] * e1[1], t[2] * e1[0] - t[0] * e1[2], t[0] * e1[1] - t[1] * e1[0]};
  const double v = (d.x * q[0] + d.y * q[1] + d.z * q[2]) * inv;
  if (v < 0.0 || u + v > 1.0) return std::nullopt;
  const double s = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  if (s < 0.0) return std::nullopt;
  return s;
}

}  // namespace

bool Interior::load(const std::filesystem::path& file, std::string& err) {
  auto raw = readFile(file);
  if (!raw || raw->size() < 12 || std::memcmp(raw->data(), "RJINT002", 8) != 0) {
    err = "bad interior file";
    return false;
  }
  uint32_t blen = 0;
  std::memcpy(&blen, raw->data() + 8, 4);
  int out = 0;
  unsigned char* body = DecompressData(raw->data() + 12, static_cast<int>(raw->size() - 12), &out);
  if (!body || static_cast<uint32_t>(out) != blen) {
    if (body) MemFree(body);
    err = "interior decompression failed";
    return false;
  }
  const std::vector<unsigned char> data(body, body + out);
  MemFree(body);
  Reader r(data);
  char magic[8];
  r.bytes(magic, 8);
  id_ = r.str();
  name_ = r.str();
  double anchor[3];
  for (double& v : anchor) v = r.get<double>();
  frame_ = rj::geo::LocalFrame({anchor[0], anchor[1], anchor[2]});
  r.get<uint32_t>();  // status
  r.get<uint32_t>();  // source index
  const uint32_t nc = r.get<uint32_t>();
  for (uint32_t k = 0; k < nc && r.ok(); ++k) {
    const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
    r.get<int32_t>();
    const float scale = r.get<float>();
    if (nv > 65535) {
      err = "bad interior chunk";
      return false;
    }
    std::vector<int16_t> qp(nv * 3);
    std::vector<int8_t> qn(nv * 4);
    r.bytes(qp.data(), qp.size() * 2);
    r.skip((4 - (nv * 6) % 4) % 4);
    r.bytes(qn.data(), qn.size());
    Mesh m{};
    m.vertexCount = static_cast<int>(nv);
    m.triangleCount = static_cast<int>(ni / 3);
    m.vertices = static_cast<float*>(MemAlloc(nv * 3 * sizeof(float)));
    m.normals = static_cast<float*>(MemAlloc(nv * 3 * sizeof(float)));
    m.colors = static_cast<unsigned char*>(MemAlloc(nv * 4));
    m.indices = static_cast<unsigned short*>(MemAlloc(ni * sizeof(unsigned short)));
    r.bytes(m.colors, nv * 4);
    r.skip(nv * 4);  // uv (unused)
    r.bytes(m.indices, ni * 2);
    r.skip((4 - (ni * 2) % 4) % 4);
    for (size_t i = 0; i < qp.size(); ++i) m.vertices[i] = static_cast<float>(qp[i]) * scale;
    for (size_t v = 0; v < nv; ++v)
      for (int a = 0; a < 3; ++a) m.normals[v * 3 + a] = static_cast<float>(qn[v * 4 + a]) / 127.0f;
    if (!r.ok()) {
      UnloadMesh(m);
      break;
    }
    UploadMesh(&m, false);
    MemFree(m.vertices);
    MemFree(m.normals);
    MemFree(m.colors);
    m.vertices = m.normals = nullptr;
    m.colors = nullptr;
    meshes_.push_back(m);
  }
  auto tris = [&](std::vector<Tri>& out_tris) {
    const uint32_t n = r.get<uint32_t>();
    if (!r.ok() || n > 10'000'000) return;
    out_tris.resize(n);
    r.bytes(out_tris.data(), n * sizeof(Tri));
  };
  tris(floors_local_);
  tris(walls_local_);
  const uint32_t ne = r.get<uint32_t>();
  for (uint32_t k = 0; k < ne && r.ok(); ++k) {
    float f[6];
    r.bytes(f, sizeof f);
    ents_local_.push_back({{f[0], f[1], f[2]}, {f[3], f[4], f[5]}});
  }
  tris(openings_local_);
  if (!r.ok()) {
    err = "truncated interior";
    unload();
    return false;
  }
  addLidsForUprightOpenings();
  return true;
}

// Some entrances close with an upright surface (a doorway at the top of the stairs) rather than a
// lid over the stairwell: seen from above it has no area, so nobody could step "over" it. Each one
// next to an entrance gets a flat lid 1.2 m deep on the side where the interior floor lies below.
void Interior::addLidsForUprightOpenings() {
  auto floorUnder = [&](double x, double y, double z_top) {
    for (const Tri& t : floors_local_)
      if (auto z = zAt(t.a, t.b, t.c, x, y); z && *z < z_top - 0.3 && *z > z_top - 12.0) return true;
    return false;
  };
  std::vector<Tri> lids;
  for (const Tri& t : openings_local_) {
    const double area = 0.5 * std::fabs((t.b[0] - t.a[0]) * (t.c[1] - t.a[1]) - (t.c[0] - t.a[0]) * (t.b[1] - t.a[1]));
    if (area > 0.05) continue;
    const float* v[3] = {t.a, t.b, t.c};
    int i0 = 0, i1 = 1;
    double dmax = -1;
    for (int i = 0; i < 3; ++i)
      for (int j = i + 1; j < 3; ++j)
        if (const double d = std::hypot(v[j][0] - v[i][0], v[j][1] - v[i][1]); d > dmax) {
          dmax = d;
          i0 = i;
          i1 = j;
        }
    if (dmax < 0.3) continue;
    const double zb = std::min({t.a[2], t.b[2], t.c[2]});
    const double mx = 0.5 * (v[i0][0] + v[i1][0]), my = 0.5 * (v[i0][1] + v[i1][1]);
    bool near_entrance = false;
    for (const auto& e : ents_local_)
      near_entrance |= std::hypot(e.first.x - mx, e.first.y - my) < 6.0 && std::fabs(e.first.z - zb) < 2.5;
    if (!near_entrance) continue;
    const double nx = -(v[i1][1] - v[i0][1]) / dmax, ny = (v[i1][0] - v[i0][0]) / dmax;
    for (const double side : {1.0, -1.0}) {
      if (!floorUnder(mx + nx * side * 0.9, my + ny * side * 0.9, zb + 0.3)) continue;
      const float dx = static_cast<float>(nx * side * 1.2), dy = static_cast<float>(ny * side * 1.2), z = static_cast<float>(zb);
      const Tri a{{v[i0][0], v[i0][1], z}, {v[i1][0], v[i1][1], z}, {v[i1][0] + dx, v[i1][1] + dy, z}};
      const Tri b{{v[i0][0], v[i0][1], z}, {v[i1][0] + dx, v[i1][1] + dy, z}, {v[i0][0] + dx, v[i0][1] + dy, z}};
      lids.push_back(a);
      lids.push_back(b);
      break;
    }
  }
  if (!lids.empty()) TraceLog(LOG_INFO, "RJ: interior %s: %zu upright entrance openings got a lid", id_.c_str(), lids.size() / 2);
  openings_local_.insert(openings_local_.end(), lids.begin(), lids.end());
}

void Interior::unload() {
  for (auto& m : meshes_) UnloadMesh(m);
  meshes_.clear();
  floors_.clear();
  walls_.clear();
  openings_.clear();
  floor_hash_.clear();
  wall_hash_.clear();
}

void Interior::place(const rj::geo::LocalFrame& origin) {
  const rj::geo::Rigid3d T = origin.transformFrom(frame_);
  model_ = rigidToRaylib(T);
  auto conv = [&](const std::vector<Tri>& src, std::vector<Tri>& dst) {
    dst.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      const float* in[3] = {src[i].a, src[i].b, src[i].c};
      float* o[3] = {dst[i].a, dst[i].b, dst[i].c};
      for (int k = 0; k < 3; ++k) {
        const auto p = T.apply({in[k][0], in[k][1], in[k][2]});
        o[k][0] = static_cast<float>(p.x);
        o[k][1] = static_cast<float>(p.y);
        o[k][2] = static_cast<float>(p.z);
      }
    }
  };
  conv(floors_local_, floors_);
  conv(walls_local_, walls_);
  conv(openings_local_, openings_);
  ents_.clear();
  for (const auto& [s, i] : ents_local_) ents_.push_back({T.apply(s), T.apply(i)});
  rebuildHash();
}

void Interior::rebuildHash() {
  floor_hash_.clear();
  wall_hash_.clear();
  x0_ = y0_ = 1e30f;
  x1_ = y1_ = -1e30f;
  auto add = [&](const std::vector<Tri>& v, std::unordered_map<int64_t, std::vector<uint32_t>>& h) {
    for (uint32_t i = 0; i < v.size(); ++i) {
      const Tri& t = v[i];
      const float mnx = std::min({t.a[0], t.b[0], t.c[0]}), mxx = std::max({t.a[0], t.b[0], t.c[0]});
      const float mny = std::min({t.a[1], t.b[1], t.c[1]}), mxy = std::max({t.a[1], t.b[1], t.c[1]});
      x0_ = std::min(x0_, mnx);
      x1_ = std::max(x1_, mxx);
      y0_ = std::min(y0_, mny);
      y1_ = std::max(y1_, mxy);
      for (int bx = static_cast<int>(std::floor(mnx / kBucket)); bx <= static_cast<int>(std::floor(mxx / kBucket)); ++bx)
        for (int by = static_cast<int>(std::floor(mny / kBucket)); by <= static_cast<int>(std::floor(mxy / kBucket)); ++by)
          h[key(bx, by)].push_back(i);
    }
  };
  add(floors_, floor_hash_);
  add(walls_, wall_hash_);
}

bool Interior::overOpening(double x, double y) const {
  for (const Tri& t : openings_) {
    const float a[3] = {t.a[0], t.a[1], 0.0f}, b[3] = {t.b[0], t.b[1], 0.0f}, c[3] = {t.c[0], t.c[1], 0.0f};
    if (zAt(a, b, c, x, y)) return true;
  }
  return false;
}

double Interior::distanceToOpening(double x, double y) const {
  double best = 1e30;
  for (const Tri& t : openings_) {
    const double cx = (t.a[0] + t.b[0] + t.c[0]) / 3.0, cy = (t.a[1] + t.b[1] + t.c[1]) / 3.0;
    best = std::min(best, std::hypot(x - cx, y - cy));
  }
  return best;
}

bool Interior::contains(double x, double y) const { return x >= x0_ && x <= x1_ && y >= y0_ && y <= y1_; }

std::optional<double> Interior::floorBelow(double x, double y, double z_top, double max_drop) const {
  auto it = floor_hash_.find(key(static_cast<int>(std::floor(x / kBucket)), static_cast<int>(std::floor(y / kBucket))));
  if (it == floor_hash_.end()) return std::nullopt;
  std::optional<double> best;
  for (uint32_t i : it->second) {
    const Tri& t = floors_[i];
    if (auto z = zAt(t.a, t.b, t.c, x, y); z && *z <= z_top && *z >= z_top - max_drop && (!best || *z > *best)) best = z;
  }
  return best;
}

void Interior::collide(rj::geo::Vec3d& p, double radius) const {
  for (int iter = 0; iter < 3; ++iter) {
    bool moved = false;
    const int bx = static_cast<int>(std::floor(p.x / kBucket)), by = static_cast<int>(std::floor(p.y / kBucket));
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy) {
        auto it = wall_hash_.find(key(bx + dx, by + dy));
        if (it == wall_hash_.end()) continue;
        for (uint32_t i : it->second) {
          const Tri& t = walls_[i];
          const float* v[3] = {t.a, t.b, t.c};
          const double zmin = std::min({t.a[2], t.b[2], t.c[2]}), zmax = std::max({t.a[2], t.b[2], t.c[2]});
          if (zmax < p.z + 0.35 || zmin > p.z + 1.7) continue;  // below knee / above head
          // Cut the triangle with the horizontal plane at chest height (clamped into its z range).
          const double h = std::clamp(p.z + 1.0, zmin + 0.01, zmax - 0.01);
          double pts[2][2];
          int n = 0;
          for (int e = 0; e < 3 && n < 2; ++e) {
            const float* a = v[e];
            const float* b = v[(e + 1) % 3];
            if ((a[2] - h) * (b[2] - h) < 0) {
              const double s = (h - a[2]) / (b[2] - a[2]);
              pts[n][0] = a[0] + (b[0] - a[0]) * s;
              pts[n][1] = a[1] + (b[1] - a[1]) * s;
              ++n;
            }
          }
          if (n < 2) continue;
          const double sx = pts[1][0] - pts[0][0], sy = pts[1][1] - pts[0][1];
          const double l2 = sx * sx + sy * sy;
          double u = l2 > 1e-9 ? ((p.x - pts[0][0]) * sx + (p.y - pts[0][1]) * sy) / l2 : 0.0;
          u = std::clamp(u, 0.0, 1.0);
          const double cx = pts[0][0] + u * sx, cy = pts[0][1] + u * sy;
          double nx = p.x - cx, ny = p.y - cy;
          const double d = std::hypot(nx, ny);
          if (d >= radius) continue;
          if (d < 1e-6) {
            nx = -sy;
            ny = sx;
          }
          const double l = std::hypot(nx, ny);
          p.x = cx + nx / l * (radius + 0.005);
          p.y = cy + ny / l * (radius + 0.005);
          moved = true;
        }
      }
    if (!moved) break;
  }
}

std::optional<double> Interior::raycast(const rj::geo::Vec3d& from, const rj::geo::Vec3d& dir, double max_d) const {
  std::optional<double> best;
  std::vector<int64_t> seen;
  const int steps = static_cast<int>(std::ceil(max_d / kBucket)) + 1;
  for (int k = 0; k <= steps; ++k) {
    const double s = std::min(max_d, k * kBucket);
    const int bx = static_cast<int>(std::floor((from.x + dir.x * s) / kBucket));
    const int by = static_cast<int>(std::floor((from.y + dir.y * s) / kBucket));
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy) {
        const int64_t kk = key(bx + dx, by + dy);
        if (std::find(seen.begin(), seen.end(), kk) != seen.end()) continue;
        seen.push_back(kk);
        for (const auto* h : {&wall_hash_, &floor_hash_}) {
          auto it = h->find(kk);
          if (it == h->end()) continue;
          const std::vector<Tri>& tris = (h == &wall_hash_) ? walls_ : floors_;
          for (uint32_t i : it->second)
            if (auto t = rayTri(from, dir, tris[i].a, tris[i].b, tris[i].c); t && *t <= max_d && (!best || *t < *best))
              best = t;
        }
      }
  }
  return best;
}

}  // namespace rjc
