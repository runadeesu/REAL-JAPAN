#include "game/road_markings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "game/traffic.hpp"
#include "game/traffic_signals.hpp"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

using rj::nav::Vec2;

constexpr double kLift = 0.02;                                  // paint just above the terrain surface
constexpr double kLineW = 0.15;                                 // centre / lane / outside lines
constexpr double kBarW = 0.45, kBarGap = 0.45, kCrossW = 4.0;  // zebra crossing (横断歩道)
constexpr double kStopW = 0.45, kStopGap = 2.0;                 // stop line (停止線), set back from the crossing
constexpr double kDash = 5.0, kDashGap = 5.0;                   // broken lines on general roads
constexpr double kSolidBeforeStop = 30.0;                       // lane lines turn solid before a signalised stop line

double compass(const Vec2& v) { return std::atan2(v.x, v.y); }
Vec2 leftOf(const Vec2& t) { return {-t.y, t.x}; }
Vec2 add(const Vec2& a, const Vec2& b, double k) { return {a.x + b.x * k, a.y + b.y * k}; }

Vec2 pointOn(const Traffic::Edge& e, double u) {
  u = std::clamp(u, 0.0, e.length);
  size_t k = static_cast<size_t>(std::upper_bound(e.cum.begin(), e.cum.end(), u) - e.cum.begin());
  k = std::clamp<size_t>(k, 1, e.pts.size() - 1);
  const auto& A = e.pts[k - 1];
  const auto& B = e.pts[k];
  const double seg = std::max(1e-6, e.cum[k] - e.cum[k - 1]);
  const double t = std::clamp((u - e.cum[k - 1]) / seg, 0.0, 1.0);
  return {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t};
}

// Unit tangent (a -> b) by central difference, so offset lines stay smooth through polyline corners.
Vec2 tangentOn(const Traffic::Edge& e, double u) {
  const Vec2 p = pointOn(e, u - 1.0), q = pointOn(e, u + 1.0);
  const double l = std::hypot(q.x - p.x, q.y - p.y);
  return l > 1e-9 ? Vec2{(q.x - p.x) / l, (q.y - p.y) / l} : Vec2{0, 1};
}

struct MeshBuilder {
  const World& world;
  std::vector<Mesh>& out;
  size_t& tris;
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;

  void flush() {
    if (idx.empty()) return;
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.texcoords2 = t2.data();
    m.colors = col.data();
    m.indices = idx.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    out.push_back(m);
    tris += idx.size() / 3;
    pos.clear();
    nrm.clear();
    uv.clear();
    t2.clear();
    col.clear();
    idx.clear();
  }

  // Ground quad a-b-c-d (origin ENU x/y), heights from the terrain; raylib space x, y up, z = -north.
  void quad(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    const Vec2 q[4] = {a, b, c, d};
    float z[4];
    for (int k = 0; k < 4; ++k) {
      const auto h = world.terrainHeight(q[k].x, q[k].y);
      if (!h) return;
      z[k] = static_cast<float>(*h + kLift);
    }
    if (pos.size() / 3 + 4 > 65000) flush();
    const auto base = static_cast<unsigned short>(pos.size() / 3);
    for (int k = 0; k < 4; ++k) {
      pos.insert(pos.end(), {static_cast<float>(q[k].x), z[k], static_cast<float>(-q[k].y)});
      nrm.insert(nrm.end(), {0.0f, 1.0f, 0.0f});
      uv.insert(uv.end(), {0.0f, 0.0f});
      t2.insert(t2.end(), {static_cast<float>(kMatMarking), 0.0f});
      col.insert(col.end(), {255, 255, 255, 255});
    }
    idx.insert(idx.end(), {base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2), base,
                           static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)});
  }

  // Generic vertex (origin ENU position / normal) for street hardware.
  unsigned short vert(double x, double y, double z, double nx, double ny, double nz, int mat, Color c) {
    const auto i = static_cast<unsigned short>(pos.size() / 3);
    pos.insert(pos.end(), {static_cast<float>(x), static_cast<float>(z), static_cast<float>(-y)});
    nrm.insert(nrm.end(), {static_cast<float>(nx), static_cast<float>(nz), static_cast<float>(-ny)});
    uv.insert(uv.end(), {0.0f, 0.0f});
    t2.insert(t2.end(), {static_cast<float>(mat), 0.0f});
    col.insert(col.end(), {c.r, c.g, c.b, 255});
    return i;
  }
  // Oriented box: centre (ENU), horizontal forward axis f (unit), half extents along right / forward / up.
  void box(double cx, double cy, double cz, const Vec2& f, double hr, double hf, double hu, int mat, Color c) {
    if (pos.size() / 3 + 24 > 65000) flush();
    const Vec2 r{f.y, -f.x};  // right of forward (compass)
    const double ax[3][3] = {{r.x, r.y, 0}, {f.x, f.y, 0}, {0, 0, 1}};
    const double he[3] = {hr, hf, hu};
    for (int a = 0; a < 3; ++a)
      for (int sgn = -1; sgn <= 1; sgn += 2) {
        const int b = (a + 1) % 3, d = (a + 2) % 3;
        unsigned short q[4];
        const double uu[4] = {-1, 1, 1, -1}, vv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
          const double su = uu[k] * (sgn > 0 ? 1 : -1), sv = vv[k];
          double p[3];
          for (int j = 0; j < 3; ++j) p[j] = ax[a][j] * he[a] * sgn + ax[b][j] * he[b] * su + ax[d][j] * he[d] * sv;
          q[k] = vert(cx + p[0], cy + p[1], cz + p[2], ax[a][0] * sgn, ax[a][1] * sgn, ax[a][2] * sgn, mat, c);
        }
        idx.insert(idx.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
      }
  }
  // Vertical 8-sided pole from z0 to z1.
  void pole(double x, double y, double z0, double z1, double r, int mat, Color c) {
    if (pos.size() / 3 + 20 > 65000) flush();
    const int n = 8;
    unsigned short first = 0;
    for (int i = 0; i <= n; ++i) {
      const double a = 2.0 * 3.14159265358979323846 * i / n, ca = std::cos(a), sa = std::sin(a);
      const auto lo = vert(x + ca * r, y + sa * r, z0, ca, sa, 0, mat, c);
      vert(x + ca * r, y + sa * r, z1, ca, sa, 0, mat, c);
      if (i == 0) first = lo;
      if (i > 0) {
        const auto p0 = static_cast<unsigned short>(lo - 2), p1 = static_cast<unsigned short>(lo - 1);
        idx.insert(idx.end(), {p0, lo, static_cast<unsigned short>(lo + 1), p0, static_cast<unsigned short>(lo + 1), p1});
      }
    }
    (void)first;
  }

  // Solid strip of width w along the edge at lateral offset `off` (left of a -> b), arc length u0..u1.
  void strip(const Traffic::Edge& e, double off, double u0, double u1, double w) {
    const int n = std::max(1, static_cast<int>(std::ceil((u1 - u0) / 2.0)));
    Vec2 pl{}, pr{};
    for (int i = 0; i <= n; ++i) {
      const double u = u0 + (u1 - u0) * i / n;
      const Vec2 nl = leftOf(tangentOn(e, u));
      const Vec2 c = add(pointOn(e, u), nl, off);
      const Vec2 L = add(c, nl, w * 0.5), R = add(c, nl, -w * 0.5);
      if (i > 0) quad(pr, R, L, pl);
      pl = L;
      pr = R;
    }
  }

  // Line with an optional dash pattern (dash phase anchored at node a so both halves agree).
  void line(const Traffic::Edge& e, double off, double u0, double u1, double w, double on = 0.0, double gap = 0.0) {
    if (u1 - u0 < 0.3) return;
    if (on <= 0.0) {
      strip(e, off, u0, u1, w);
      return;
    }
    const double per = on + gap;
    for (double s = std::floor(u0 / per) * per; s < u1; s += per) {
      const double a = std::max(s, u0), b = std::min(s + on, u1);
      if (b - a > 0.3) strip(e, off, a, b, w);
    }
  }

  // Bar across the road at arc length u, lateral offsets o0..o1, width w along the road.
  void across(const Traffic::Edge& e, double u, double o0, double o1, double w) {
    const Vec2 t = tangentOn(e, u), nl = leftOf(t), p = pointOn(e, u);
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(o1 - o0) / 2.0)));
    for (int i = 0; i < n; ++i) {
      const double a = o0 + (o1 - o0) * i / n, b = o0 + (o1 - o0) * (i + 1) / n;
      const Vec2 ca = add(p, nl, a), cb = add(p, nl, b);
      quad(add(ca, t, -w * 0.5), add(cb, t, -w * 0.5), add(cb, t, w * 0.5), add(ca, t, w * 0.5));
    }
  }
};

bool surveyedAt(const World& world, const Vec2& p, double r) {
  return world.onCrosswalk(p.x, p.y) || world.hasSurveyedMarking(p.x, p.y, r);
}

// Nearest signal head of the given kind within max_d; returns its group or -1.
int nearestGroup(const TrafficSignals& signals, const Vec2& p, int kind, double max_d) {
  int g = -1;
  double best = max_d;
  for (const auto& h : signals.heads()) {
    if (h.kind != kind) continue;
    const double d = std::hypot(h.pos.x - p.x, h.pos.y - p.y);
    if (d < best) {
      best = d;
      g = h.group;
    }
  }
  return g;
}

void attachSignal(Crossing& c, const TrafficSignals& signals, int known_group = -1) {
  c.group = known_group >= 0 ? known_group : nearestGroup(signals, c.center, 1, 30.0);
  if (c.group < 0) c.group = nearestGroup(signals, c.center, 0, 35.0);
  if (c.group >= 0) c.phase = signals.phaseForAxis(c.group, c.heading);
}

// Surveyed PLATEAU crosswalk areas (frn 1110) grouped into crossings.
void surveyedCrossings(const World& world, const TrafficSignals& signals, std::vector<Crossing>& out) {
  struct Tri {
    Vec2 v[3];
    double x0, y0, x1, y1;
  };
  std::vector<Tri> tris;
  for (const auto& [code, c] : world.cells())
    for (size_t t = 0; t + 8 < c->cross.size(); t += 9) {
      Tri tr{};
      tr.x0 = tr.y0 = 1e30;
      tr.x1 = tr.y1 = -1e30;
      for (int k = 0; k < 3; ++k) {
        tr.v[k] = {c->cross[t + k * 3], c->cross[t + k * 3 + 1]};
        tr.x0 = std::min(tr.x0, tr.v[k].x);
        tr.x1 = std::max(tr.x1, tr.v[k].x);
        tr.y0 = std::min(tr.y0, tr.v[k].y);
        tr.y1 = std::max(tr.y1, tr.v[k].y);
      }
      tris.push_back(tr);
    }
  if (tris.empty()) return;
  // Union-find over triangles whose boxes (grown by 0.6 m: bar gaps are 0.45 m) overlap; 4 m buckets.
  std::vector<int> parent(tris.size());
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](int a) {
    while (parent[static_cast<size_t>(a)] != a) a = parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(parent[static_cast<size_t>(a)])];
    return a;
  };
  constexpr double kGrow = 0.6, kB = 4.0;
  std::unordered_map<int64_t, std::vector<int>> buckets;
  auto key = [](int x, int y) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(y); };
  for (size_t i = 0; i < tris.size(); ++i) {
    const Tri& a = tris[i];
    for (int bx = static_cast<int>(std::floor((a.x0 - kGrow) / kB)); bx <= static_cast<int>(std::floor((a.x1 + kGrow) / kB)); ++bx)
      for (int by = static_cast<int>(std::floor((a.y0 - kGrow) / kB)); by <= static_cast<int>(std::floor((a.y1 + kGrow) / kB)); ++by) {
        auto& b = buckets[key(bx, by)];
        for (int j : b) {
          const Tri& o = tris[static_cast<size_t>(j)];
          if (a.x0 - kGrow <= o.x1 && o.x0 - kGrow <= a.x1 && a.y0 - kGrow <= o.y1 && o.y0 - kGrow <= a.y1)
            parent[static_cast<size_t>(find(static_cast<int>(i)))] = find(j);
        }
        b.push_back(static_cast<int>(i));
      }
  }
  std::unordered_map<int, size_t> cluster;
  for (size_t i = 0; i < tris.size(); ++i) {
    const int r = find(static_cast<int>(i));
    auto it = cluster.find(r);
    if (it == cluster.end()) {
      it = cluster.emplace(r, out.size()).first;
      out.push_back({});
    }
    Crossing& c = out[it->second];
    c.polys.push_back({tris[i].v[0], tris[i].v[1], tris[i].v[2]});
  }
  for (auto& [r, ci] : cluster) {
    Crossing& c = out[ci];
    // Centre and principal axis of the area: people walk along its long side (across the road).
    double sx = 0, sy = 0;
    size_t n = 0;
    for (const auto& p : c.polys)
      for (const auto& v : p) {
        sx += v.x;
        sy += v.y;
        ++n;
      }
    c.center = {sx / static_cast<double>(n), sy / static_cast<double>(n)};
    double cxx = 0, cxy = 0, cyy = 0;
    for (const auto& p : c.polys)
      for (const auto& v : p) {
        const double dx = v.x - c.center.x, dy = v.y - c.center.y;
        cxx += dx * dx;
        cxy += dx * dy;
        cyy += dy * dy;
      }
    const double ang = 0.5 * std::atan2(2 * cxy, cxx - cyy);  // major axis angle from +x (east)
    const Vec2 ax{std::cos(ang), std::sin(ang)}, ay{-ax.y, ax.x};
    c.heading = compass(ax);
    // The walk area is the oriented box of the painted area, reaching 1 m onto the sidewalks
    // (painted bars are narrower than the 2 m walk grid).
    double a0 = 1e30, a1 = -1e30, b0 = 1e30, b1 = -1e30;
    for (const auto& p : c.polys)
      for (const auto& v : p) {
        const double da = (v.x - c.center.x) * ax.x + (v.y - c.center.y) * ax.y;
        const double db = (v.x - c.center.x) * ay.x + (v.y - c.center.y) * ay.y;
        a0 = std::min(a0, da);
        a1 = std::max(a1, da);
        b0 = std::min(b0, db);
        b1 = std::max(b1, db);
      }
    a0 -= 1.0;
    a1 += 1.0;
    if (b1 - b0 < 3.0) {
      const double m = (b0 + b1) * 0.5;
      b0 = m - 1.5;
      b1 = m + 1.5;
    }
    auto corner = [&](double a, double b) { return add(add(c.center, ax, a), ay, b); };
    c.polys = {{corner(a0, b0), corner(a1, b0), corner(a1, b1), corner(a0, b1)}};
    c.estimated = false;
    attachSignal(c, signals);
  }
}

}  // namespace

RoadMarkings::~RoadMarkings() { unload(); }

void RoadMarkings::unload() {
  for (auto& m : meshes_) UnloadMesh(m);
  meshes_.clear();
  crossings_.clear();
  n_est_crossings_ = 0;
  n_est_signals_ = 0;
  tris_ = 0;
}

void RoadMarkings::build(Traffic& traffic, TrafficSignals& signals, const World& world) {
  unload();
  traffic.clearRoadMarkingState();
  signals.clearEstimated();
  traffic.assignSignalGroups(signals);  // real signal groups only
  surveyedCrossings(world, signals, crossings_);
  const auto& nodes = traffic.nodes();
  const auto& edges = traffic.edges();
  MeshBuilder mb{world, meshes_, tris_, {}, {}, {}, {}, {}, {}};

  // --- estimated signals: major junctions (>= 2 arms of >= 7.5 m) with no surveyed heads nearby.
  // Skeleton nodes of one large junction area are merged (within 28 m) into one intersection.
  {
    std::vector<int> cand;
    for (size_t n = 0; n < nodes.size(); ++n) {
      const auto& N = nodes[n];
      if (N.signal_group >= 0 || N.edges.size() < 3) continue;
      int major = 0;
      for (int ei : N.edges) major += edges[static_cast<size_t>(ei)].width >= 7.5f ? 1 : 0;
      if (major >= 2) cand.push_back(static_cast<int>(n));
    }
    std::vector<int> parent(cand.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int a) {
      while (parent[static_cast<size_t>(a)] != a) a = parent[static_cast<size_t>(a)];
      return a;
    };
    for (size_t i = 0; i < cand.size(); ++i)
      for (size_t j = i + 1; j < cand.size(); ++j) {
        const auto& A = nodes[static_cast<size_t>(cand[i])].pos;
        const auto& B = nodes[static_cast<size_t>(cand[j])].pos;
        if (std::hypot(A.x - B.x, A.y - B.y) < 28.0) parent[static_cast<size_t>(find(static_cast<int>(i)))] = find(static_cast<int>(j));
      }
    std::unordered_map<int, int> gid;
    for (size_t i = 0; i < cand.size(); ++i) {
      const int r = find(static_cast<int>(i));
      auto it = gid.find(r);
      if (it == gid.end()) it = gid.emplace(r, signals.addEstimatedGroup()).first;
      traffic.setEstimatedGroup(cand[i], it->second);
    }
    traffic.assignSignalGroups(signals);  // real + estimated
  }

  // --- junction arms: crossing / stop line positions for every edge end at a signalised junction ---
  struct Arm {
    size_t edge;
    int end;
    int group;
    bool est;
    double d, stopd, u, us;  // crossing centre / stop line from the node; arc positions on the edge
    Vec2 center, t, nl;      // t points away from the junction
    bool surveyed;
  };
  std::vector<Arm> arms;
  std::vector<std::array<float, 2>> trim(edges.size(), {0.0f, 0.0f});
  std::vector<std::array<bool, 2>> signal_end(edges.size(), {false, false});
  std::vector<bool> internal(edges.size(), false);
  for (size_t i = 0; i < edges.size(); ++i) {
    const Traffic::Edge& e = edges[i];
    if (e.a == e.b || e.pts.size() < 2) continue;
    const int ga = nodes[static_cast<size_t>(e.a)].signal_group, gb = nodes[static_cast<size_t>(e.b)].signal_group;
    if (ga >= 0 && ga == gb) {
      internal[i] = true;  // inside one junction area: no crossing, no lines
      continue;
    }
    for (int end = 0; end < 2; ++end) {
      const Traffic::Node& N = nodes[static_cast<size_t>(end ? e.b : e.a)];
      const size_t deg = N.edges.size();
      double other_half = 0.0;
      for (int j : N.edges)
        if (static_cast<size_t>(j) != i) other_half = std::max(other_half, edges[static_cast<size_t>(j)].width * 0.5);
      if (deg >= 3) trim[i][end] = static_cast<float>(other_half + 1.0);
      if (N.signal_group < 0 || deg < 3 || e.width < 4.0f) continue;
      Arm a{};
      a.edge = i;
      a.end = end;
      a.group = N.signal_group;
      a.est = N.est_group >= 0;
      a.d = other_half + 1.5 + kCrossW * 0.5;
      a.stopd = a.d + kCrossW * 0.5 + kStopGap + kStopW * 0.5;
      if (e.length < a.stopd + 6.0) continue;
      a.u = end ? e.length - a.d : a.d;
      a.us = end ? e.length - a.stopd : a.stopd;
      a.center = pointOn(e, a.u);
      const Vec2 tab = tangentOn(e, a.u);
      a.t = end ? Vec2{-tab.x, -tab.y} : tab;
      a.nl = leftOf(tab);
      a.surveyed = surveyedAt(world, a.center, 3.0);
      trim[i][end] = static_cast<float>(a.stopd + (a.surveyed ? 0.0 : kStopW));
      signal_end[i][end] = !a.surveyed;
      arms.push_back(a);
    }
  }

  // --- estimated signal hardware: far-side vehicle heads over the departure lanes, pedestrian heads
  // at both ends of each crossing (Japanese practice; exact positions are inferred) ---
  const Color kPoleCol{186, 186, 182, 255}, kHousingCol{150, 152, 150, 255};
  auto phaseOf = [&](int group, double facing) {
    // Phase 0 serves the axis of the widest arm of the junction.
    double ref = facing, best_w = -1;
    for (const Arm& a : arms)
      if (a.group == group && edges[a.edge].width > best_w) {
        best_w = edges[a.edge].width;
        ref = compass(a.t);
      }
    return std::fabs(std::remainder(facing - ref, 3.14159265358979323846)) < 3.14159265358979323846 / 4 ? 0 : 1;
  };
  auto groundAt = [&](const Vec2& p) {
    const auto h = world.surfaceHeight(p.x, p.y);
    return h ? *h : 0.0;
  };
  for (const Arm& a : arms) {
    if (!a.est) continue;
    const Traffic::Edge& e = edges[a.edge];
    // Vehicles arrive along -a.t. Far side: the arm that continues most straight.
    const Vec2 tin{-a.t.x, -a.t.y};
    const Arm* opp = nullptr;
    double best = 0.9;  // ~50 deg
    for (const Arm& o : arms)
      if (o.group == a.group && &o != &a) {
        const double dev = std::fabs(std::remainder(compass(o.t) - compass(tin), 2 * 3.14159265358979323846));
        if (dev < best) {
          best = dev;
          opp = &o;
        }
      }
    Vec2 pole_p, head_p, face;
    double head_h = 4.9;
    if (opp) {
      const Traffic::Edge& oe = edges[opp->edge];
      const double off = kCrossW * 0.5 + 1.2;
      const Vec2 base = add(opp->center, opp->t, off);
      const Vec2 left = leftOf(opp->t);  // left of the departing traffic
      pole_p = add(base, left, oe.width * 0.5 + 0.5);
      head_p = add(base, left, std::min(oe.width * 0.5 - 0.5, oe.lanes * oe.lane_w * 0.5));
      face = {-opp->t.x, -opp->t.y};
    } else {
      // T-junction stem: on the far sidewalk across the junction.
      const Traffic::Node& N = nodes[static_cast<size_t>(a.end ? e.b : e.a)];
      double other_half = 0.0;
      for (int j : N.edges)
        if (static_cast<size_t>(j) != a.edge) other_half = std::max(other_half, edges[static_cast<size_t>(j)].width * 0.5);
      const Vec2 np{N.pos.x, N.pos.y};
      pole_p = add(np, tin, other_half + 2.0);
      head_p = add(np, tin, other_half + 1.3);
      face = a.t;
      head_h = 3.6;
    }
    const double g = groundAt(pole_p);
    const double hz = g + head_h;
    mb.pole(pole_p.x, pole_p.y, g - 0.3, hz + 0.35, 0.11, kMatMetal, kPoleCol);
    const double al = std::hypot(head_p.x - pole_p.x, head_p.y - pole_p.y);
    if (al > 0.3) {
      const Vec2 ad{(head_p.x - pole_p.x) / al, (head_p.y - pole_p.y) / al};
      mb.box((pole_p.x + head_p.x) * 0.5, (pole_p.y + head_p.y) * 0.5, hz + 0.3, ad, 0.05, al * 0.5, 0.05, kMatMetal, kPoleCol);
    }
    // 3-lamp horizontal housing (lenses are drawn by the renderer at +0.17 m along the facing).
    const double len = 1.25, sp = std::clamp(len / 3.0, 0.3, 0.45);
    mb.box(head_p.x, head_p.y, hz, face, len * 0.5, 0.14, 0.2, kMatMetal, kHousingCol);
    const Vec2 rgt{face.y, -face.x};
    for (int k = -1; k <= 1; ++k) {  // visors
      const Vec2 v = add(add(head_p, rgt, k * sp), face, 0.26);
      mb.box(v.x, v.y, hz + 0.16, face, 0.15, 0.12, 0.012, kMatMetal, kHousingCol);
    }
    TrafficSignals::Head h{};
    h.pos = {head_p.x, head_p.y, hz};
    h.facing = static_cast<float>(compass(face));
    h.length = static_cast<float>(len);
    h.kind = 0;
    h.group = a.group;
    h.phase = phaseOf(a.group, h.facing);
    signals.addEstimatedHead(h);
    ++n_est_signals_;
  }
  for (const Arm& a : arms) {
    if (!a.est || a.surveyed) continue;
    const Traffic::Edge& e = edges[a.edge];
    for (int side = -1; side <= 1; side += 2) {
      const Vec2 base = add(add(a.center, a.nl, side * (e.width * 0.5 + 0.8)), a.t, kCrossW * 0.5 + 0.4);
      const Vec2 face{-a.nl.x * side, -a.nl.y * side};  // towards the people waiting at the other end
      const Vec2 pole_p = add(base, face, -0.22);
      const double g = groundAt(pole_p);
      mb.pole(pole_p.x, pole_p.y, g - 0.3, g + 3.25, 0.07, kMatMetal, kPoleCol);
      mb.box(base.x, base.y, g + 2.75, face, 0.17, 0.12, 0.37, kMatMetal, kHousingCol);
      TrafficSignals::Head h{};
      h.pos = {base.x, base.y, g + 2.75};
      h.facing = static_cast<float>(compass(face));
      h.length = 0.7f;
      h.kind = 1;
      h.group = a.group;
      h.phase = phaseOf(a.group, h.facing);
      signals.addEstimatedHead(h);
      ++n_est_signals_;
    }
  }

  // --- crossings and stop lines ---
  auto crossingRect = [](const Traffic::Edge& e, double u) {
    const Vec2 t = tangentOn(e, u), nl = leftOf(t), p = pointOn(e, u);
    const double h = e.width * 0.5 + 1.0;  // reach onto the sidewalk so the walk network connects
    return std::vector<Vec2>{add(add(p, t, -kCrossW / 2), nl, -h), add(add(p, t, kCrossW / 2), nl, -h),
                             add(add(p, t, kCrossW / 2), nl, h), add(add(p, t, -kCrossW / 2), nl, h)};
  };
  auto zebra = [&](const Traffic::Edge& e, double u) {
    const double half = e.width * 0.5 - 0.3;
    // Bars run along the road (45 cm wide, 45 cm apart) and span the 4 m crossing width.
    for (double off = -half + kBarW * 0.5; off <= half - kBarW * 0.5 + 1e-6; off += kBarW + kBarGap)
      mb.line(e, off, u - kCrossW * 0.5, u + kCrossW * 0.5, kBarW);
  };
  for (const Arm& a : arms) {
    if (a.surveyed) continue;  // surveyed markings (PLATEAU) are drawn from the street detail
    const Traffic::Edge& e = edges[a.edge];
    zebra(e, a.u);
    const double half = e.width * 0.5 - 0.3;
    // Left-hand traffic: vehicles arriving at node b use the lanes left of a -> b (positive offsets).
    mb.across(e, a.us, 0.0, a.end ? half : -half, kStopW);
    traffic.setStopDistance(static_cast<int>(a.edge), a.end == 1, static_cast<float>(a.stopd));
    Crossing c;
    c.polys.push_back(crossingRect(e, a.u));
    c.center = a.center;
    c.heading = compass(a.nl);
    c.estimated = true;
    attachSignal(c, signals, a.group);
    crossings_.push_back(std::move(c));
    ++n_est_crossings_;
  }
  // --- mid-block signalised crossings: surveyed signal groups with no junction node of their own ---
  {
    std::unordered_map<int, std::pair<Vec2, int>> cent;  // group -> (sum, count)
    for (const auto& h : signals.heads()) {
      if (h.estimated) continue;
      auto& s = cent[h.group];
      s.first.x += h.pos.x;
      s.first.y += h.pos.y;
      ++s.second;
    }
    for (const auto& n : nodes)
      if (n.signal_group >= 0 && n.edges.size() >= 3) cent.erase(n.signal_group);
    for (const auto& [g, s] : cent) {
      const Vec2 c{s.first.x / s.second, s.first.y / s.second};
      int best_e = -1;
      double best_d = 15.0, best_u = 0.0;
      for (size_t i = 0; i < edges.size(); ++i) {
        const Traffic::Edge& e = edges[i];
        if (e.width < 4.0f || e.pts.size() < 2) continue;
        for (size_t k = 1; k < e.pts.size(); ++k) {
          const double ax = e.pts[k - 1].x, ay = e.pts[k - 1].y, vx = e.pts[k].x - ax, vy = e.pts[k].y - ay;
          const double l2 = vx * vx + vy * vy;
          const double t = l2 > 0 ? std::clamp(((c.x - ax) * vx + (c.y - ay) * vy) / l2, 0.0, 1.0) : 0.0;
          const double d = std::hypot(c.x - (ax + t * vx), c.y - (ay + t * vy));
          if (d < best_d) {
            best_d = d;
            best_e = static_cast<int>(i);
            best_u = e.cum[k - 1] + t * std::sqrt(l2);
          }
        }
      }
      if (best_e < 0) continue;
      const Traffic::Edge& e = edges[static_cast<size_t>(best_e)];
      const double off = kCrossW * 0.5 + kStopGap + kStopW * 0.5;
      if (best_u < off + 2.0 || best_u > e.length - off - 2.0) continue;
      const Vec2 center = pointOn(e, best_u);
      if (surveyedAt(world, center, 3.0)) continue;
      zebra(e, best_u);
      const double half = e.width * 0.5 - 0.3;
      mb.across(e, best_u - off, 0.0, half, kStopW);   // arriving towards b
      mb.across(e, best_u + off, 0.0, -half, kStopW);  // arriving towards a
      Crossing cr;
      cr.polys.push_back(crossingRect(e, best_u));
      cr.center = center;
      cr.heading = compass(leftOf(tangentOn(e, best_u)));
      cr.estimated = true;
      attachSignal(cr, signals, g);
      crossings_.push_back(std::move(cr));
      ++n_est_crossings_;
    }
  }
  // --- longitudinal lines ---
  for (size_t i = 0; i < edges.size(); ++i) {
    const Traffic::Edge& e = edges[i];
    if (internal[i] || e.width < 5.5f || e.pts.size() < 2 || e.length < 12.0) continue;  // no centre line on narrow streets
    const double u0 = trim[i][0], u1 = e.length - trim[i][1];
    if (u1 - u0 < 4.0) continue;
    // Roads PLATEAU already covers (surveyed lines) are left to the street detail.
    int hit = 0, tot = 0;
    for (double u = u0; u <= u1; u += 5.0, ++tot) {
      const Vec2 p = pointOn(e, u);
      if (world.hasSurveyedMarking(p.x, p.y, e.width * 0.5)) ++hit;
    }
    if (tot > 0 && hit * 5 > tot) continue;
    const double lw = e.lane_w, half = e.width * 0.5;
    mb.line(e, 0.0, u0, u1, kLineW, e.lanes >= 2 ? 0.0 : kDash, kDashGap);  // centre line
    for (int k = 1; k < e.lanes; ++k)
      for (int side = -1; side <= 1; side += 2) {
        const double off = side * k * lw;
        // side +1 carries a -> b traffic (arrives at b), side -1 arrives at a
        const bool sig = side > 0 ? signal_end[i][1] : signal_end[i][0];
        double s0 = u0, s1 = u1;
        if (sig && side > 0) {
          const double sw = std::max(u0, u1 - kSolidBeforeStop);
          mb.line(e, off, sw, u1, kLineW);
          s1 = sw;
        } else if (sig) {
          const double sw = std::min(u1, u0 + kSolidBeforeStop);
          mb.line(e, off, u0, sw, kLineW);
          s0 = sw;
        }
        mb.line(e, off, s0, s1, kLineW, kDash, kDashGap);
      }
    const double oo = std::min(e.lanes * lw, half - 0.3);
    if (oo > 1.5) {
      mb.line(e, oo, u0, u1, kLineW);
      mb.line(e, -oo, u0, u1, kLineW);
    }
  }
  mb.flush();
  TraceLog(LOG_INFO, "RJ: road markings: %zu crossings (%zu estimated), %d estimated signal groups (%zu heads), %zu tris",
           crossings_.size(), n_est_crossings_, signals.estimatedGroups(), n_est_signals_, tris_);
}

}  // namespace rjc
