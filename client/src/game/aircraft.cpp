#include "game/aircraft.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "platform/paths.hpp"
#include "raymath.h"
#include "world/coords.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kG = 9.81;
using V3 = rj::geo::Vec3d;
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) {
  const double l = len(a);
  return l > 1e-12 ? mul(a, 1.0 / l) : V3{0, 0, 0};
}
double wrap(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}
// Round the corners of a ground path (taxiing aircraft turn on arcs, not on the spot).
std::vector<V3> fillet(const std::vector<V3>& p, double r) {
  if (p.size() < 3) return p;
  std::vector<V3> out{p.front()};
  for (size_t i = 1; i + 1 < p.size(); ++i) {
    const V3 a = sub(p[i - 1], p[i]), b = sub(p[i + 1], p[i]);
    const double la = std::hypot(a.x, a.y), lb = std::hypot(b.x, b.y);
    if (la < 1e-3 || lb < 1e-3) continue;
    const double cang = std::clamp((a.x * b.x + a.y * b.y) / (la * lb), -1.0, 1.0);
    const double ang = std::acos(cang);  // interior angle
    if (ang > kPi - 0.05) {
      out.push_back(p[i]);
      continue;
    }
    const double t = std::min({r / std::tan(ang / 2), la * 0.45, lb * 0.45});
    const V3 s = add(p[i], mul(a, t / la)), e = add(p[i], mul(b, t / lb));
    // quadratic Bezier through the corner (close to an arc for these turns)
    for (int k = 0; k <= 8; ++k) {
      const double u = k / 8.0;
      const V3 q = add(add(mul(s, (1 - u) * (1 - u)), mul(p[i], 2 * u * (1 - u))), mul(e, u * u));
      out.push_back(q);
    }
  }
  out.push_back(p.back());
  return out;
}
// regional jet reference point above the ground with the gear down
constexpr double kJetRefZ = 2.4;
}  // namespace

// ------------------------------------------------------------------------------------------------
bool Aviation::load(const std::filesystem::path& transport, std::string& err) {
  auto text = readText(transport);
  if (!text) {
    err = "no transport.txt";
    return false;
  }
  std::istringstream in(*text);
  std::string line;
  airports_.clear();
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "airport") {  // starts the next airport's block ("airport <name>|<english>")
      Airport A;
      std::string names;
      std::getline(ls, names);
      names.erase(0, names.find_first_not_of(' '));
      const auto bar = names.find('|');
      A.name = names.substr(0, bar);
      if (bar != std::string::npos) A.name_en = names.substr(bar + 1);
      airports_.push_back(A);
      continue;
    }
    if (k != "runway" && k != "taxiway" && k != "connector" && k != "apronlink" && k != "apronlane" && k != "stand" && k != "terminal") continue;
    std::vector<double> v;
    double x;
    while (ls >> x) v.push_back(x);
    if (airports_.empty()) airports_.push_back(Airport{});  // (older files: a single airport, no header)
    airports_.back().raw.push_back({k, v});
  }
  for (auto& A : airports_) {
    bool rwy = false, twy = false;
    int stands = 0;
    for (const auto& [k, v] : A.raw) {
      rwy |= k == "runway";
      twy |= k == "taxiway";
      stands += k == "stand";
    }
    A.ok = rwy && twy && stands > 0;
  }
  airports_.erase(std::remove_if(airports_.begin(), airports_.end(), [](const Airport& A) { return !A.ok; }), airports_.end());
  if (airports_.empty()) {
    err = "transport.txt has no airport taxi network";
    return false;
  }
  return true;
}

void Aviation::place(const World& world) {
  auto G = [&](double la, double lo) {
    V3 p = world.toLocal({la, lo, 0.0});
    if (auto h = world.terrainHeight(p.x, p.y)) p.z = *h;
    return p;
  };
  for (Airport& A : airports_) {
    A.connectors.clear();
    A.apron_links.clear();
    A.stands.clear();
    for (const auto& [k, v] : A.raw) {
      if (k == "runway" && v.size() >= 5) {
        A.rwy_a = G(v[0], v[1]);
        A.rwy_b = G(v[2], v[3]);
        A.rwy_width = v[4];
      } else if (k == "taxiway" && v.size() >= 4) {
        A.twy_a = G(v[0], v[1]);
        A.twy_b = G(v[2], v[3]);
      } else if (k == "connector" && v.size() >= 4) {
        A.connectors.push_back({G(v[0], v[1]), G(v[2], v[3])});
      } else if (k == "apronlink" && v.size() >= 4) {
        A.apron_links.push_back({G(v[0], v[1]), G(v[2], v[3])});
      } else if (k == "apronlane" && v.size() >= 4) {
        A.lane_a = G(v[0], v[1]);
        A.lane_b = G(v[2], v[3]);
      } else if (k == "stand" && v.size() >= 3) {
        A.stands.push_back({G(v[0], v[1]), v[2]});
      } else if (k == "terminal" && v.size() >= 2) {
        A.terminal = G(v[0], v[1]);
      }
    }
    // light aircraft parking: west end of the apron, in front of the hangars, nose to the runway
    const V3 ux = norm(sub(A.rwy_b, A.rwy_a)), nx = norm(sub(A.twy_a, A.rwy_a));
    const double at = dot(sub(A.terminal, A.rwy_a), ux);
    const double tw = len(sub(A.twy_a, A.rwy_a));
    A.ga_stand = add(add(A.rwy_a, mul(ux, at - 330.0)), mul(nx, tw + 160.0));
    if (auto h = world.terrainHeight(A.ga_stand.x, A.ga_stand.y)) A.ga_stand.z = *h;
    A.ga_heading = std::atan2(-nx.x, -nx.y) / kDeg;
  }
  if (!placed_) {
    // one aircraft on the route, starting at each end (the flights alternate)
    Airliner a;
    a.id = 1;
    a.from = 0;
    a.to = airports_.size() > 1 ? 1 : 0;
    a.stand = std::min<int>(1, static_cast<int>(airports_[0].stands.size()) - 1);
    a.phase = Airliner::Phase::AtStand;
    a.timer = 150.0;
    jets_.push_back(a);
    if (airports_.size() > 1) {
      Airliner b;
      b.id = 2;
      b.from = 1;
      b.to = 0;
      b.stand = 0;
      b.phase = Airliner::Phase::AtStand;
      b.timer = 520.0;
      jets_.push_back(b);
    }
    plane_.reset(airports_[0].ga_stand, airports_[0].ga_heading, world);
  }
  for (auto& a : jets_) {  // (moving jets are carried over a rebase by shiftOrigin)
    const Airport& A = airports_[static_cast<size_t>(a.from)];
    const auto& st = A.stands[static_cast<size_t>(std::min<int>(a.stand, static_cast<int>(A.stands.size()) - 1))];
    if (a.phase == Airliner::Phase::AtStand) {
      a.pos = {st.pos.x, st.pos.y, st.pos.z + kJetRefZ};
      a.yaw = static_cast<float>(st.heading * kDeg);
      a.path.clear();
    }
  }
  heights_ok_ = false;
  height_retry_ = 0;
  placed_ = true;
}

void Aviation::resetPlane(const World& world) { plane_.reset(airports_[0].ga_stand, airports_[0].ga_heading, world); }

rj::geo::Vec3d Aviation::landside(int airport) const {
  const Airport& ap = airports_[static_cast<size_t>(std::clamp<int>(airport, 0, static_cast<int>(airports_.size()) - 1))];
  const V3 nxv{ap.twy_a.x - ap.rwy_a.x, ap.twy_a.y - ap.rwy_a.y, 0};
  const double nl = std::max(1.0, std::hypot(nxv.x, nxv.y));
  return {ap.terminal.x + nxv.x / nl * 140.0, ap.terminal.y + nxv.y / nl * 140.0, ap.terminal.z};
}

int Aviation::airportNear(const rj::geo::Vec3d& p, double r) const {
  for (int i = 0; i < static_cast<int>(airports_.size()); ++i) {
    const V3 l = landside(i);
    if (std::hypot(p.x - l.x, p.y - l.y) < r) return i;
  }
  return -1;
}

void Aviation::shiftOrigin(const rj::geo::Rigid3d& X) {
  for (auto& a : jets_) {
    a.pos = X.apply(a.pos);
    for (auto& p : a.path) p = X.apply(p);
  }
  plane_.shiftOrigin(X);
}

void LightPlane::shiftOrigin(const rj::geo::Rigid3d& X) {
  auto rot = [&](const V3& v) {
    return V3{X.R[0] * v.x + X.R[1] * v.y + X.R[2] * v.z, X.R[3] * v.x + X.R[4] * v.y + X.R[5] * v.z, X.R[6] * v.x + X.R[7] * v.y + X.R[8] * v.z};
  };
  pos_ = X.apply(pos_);
  cam_pos_ = X.apply(cam_pos_);
  vel_ = rot(vel_);
  f_ = rot(f_);
  r_ = rot(r_);
  u_ = rot(u_);
}

void Aviation::setPath(Airliner& a, std::vector<V3> pts) const {
  a.path = std::move(pts);
  a.cum.assign(a.path.size(), 0.0);
  for (size_t i = 1; i < a.path.size(); ++i) a.cum[i] = a.cum[i - 1] + len(sub(a.path[i], a.path[i - 1]));
  a.s = 0;
}

void Aviation::pointAt(const Airliner& a, double s, V3& p, double& heading, double& grade) const {
  s = std::clamp(s, 0.0, a.cum.back());
  size_t k = static_cast<size_t>(std::upper_bound(a.cum.begin(), a.cum.end(), s) - a.cum.begin());
  k = std::clamp<size_t>(k, 1, a.path.size() - 1);
  const V3& A = a.path[k - 1];
  const V3& B = a.path[k];
  const double seg = std::max(1e-6, a.cum[k] - a.cum[k - 1]);
  const double t = std::clamp((s - a.cum[k - 1]) / seg, 0.0, 1.0);
  p = add(A, mul(sub(B, A), t));
  heading = std::atan2(B.x - A.x, B.y - A.y);
  grade = (B.z - A.z) / std::max(1e-6, std::hypot(B.x - A.x, B.y - A.y));
}

void Aviation::buildTaxiOut(Airliner& a) const {
  const Airport& A = airports_[static_cast<size_t>(a.from)];
  const V3 ux = norm(sub(A.rwy_b, A.rwy_a)), nx = norm(sub(A.twy_a, A.rwy_a));
  const double tw = len(sub(A.twy_a, A.rwy_a));
  auto along = [&](const V3& p) { return dot(sub(p, A.rwy_a), ux); };
  auto at = [&](double s, double n) {
    V3 p = add(add(A.rwy_a, mul(ux, s)), mul(nx, n));
    p.z = A.rwy_a.z + (A.rwy_b.z - A.rwy_a.z) * std::clamp(s / std::max(1.0, len(sub(A.rwy_b, A.rwy_a))), 0.0, 1.0);
    return p;
  };
  const auto& st = A.stands[static_cast<size_t>(a.stand)];
  const double s_st = along(st.pos);
  const double lane_n = dot(sub(A.lane_a, A.rwy_a), nx);
  const double link_s = A.apron_links.empty() ? s_st : along(A.apron_links.front().first);
  const double conn_s = A.connectors.empty() ? 150.0 : along(A.connectors.front().first);
  std::vector<V3> p = {at(s_st, lane_n), at(link_s, lane_n), at(link_s, tw), at(conn_s, tw), at(conn_s, 0.0), at(conn_s + 60.0, 0.0)};
  for (auto& q : p) q.z += kJetRefZ;
  setPath(a, fillet(p, 35.0));
}

void Aviation::buildTaxiIn(Airliner& a, double stop_along) const {
  const Airport& A = airports_[static_cast<size_t>(a.to)];
  const V3 ux = norm(sub(A.rwy_b, A.rwy_a)), nx = norm(sub(A.twy_a, A.rwy_a));
  const double tw = len(sub(A.twy_a, A.rwy_a));
  auto along = [&](const V3& p) { return dot(sub(p, A.rwy_a), ux); };
  auto at = [&](double s, double n) {
    V3 p = add(add(A.rwy_a, mul(ux, s)), mul(nx, n));
    p.z = A.rwy_a.z + kJetRefZ;
    return p;
  };
  // first runway exit beyond the stopping point
  double exit_s = along(A.rwy_b) - 80.0;
  for (const auto& c : A.connectors) {
    const double s = along(c.first);
    if (s > stop_along + 40.0) {
      exit_s = s;
      break;
    }
  }
  // a stand not taken by the other aircraft
  int stand = 0;
  for (int k = 0; k < static_cast<int>(A.stands.size()); ++k) {
    bool used = false;
    for (const auto& o : jets_)
      if (&o != &a && o.phase == Airliner::Phase::AtStand && o.from == a.to && o.stand == k) used = true;
    if (!used) {
      stand = k;
      break;
    }
  }
  a.stand = stand;
  const auto& st = A.stands[static_cast<size_t>(stand)];
  const double s_st = along(st.pos);
  const double lane_n = dot(sub(A.lane_a, A.rwy_a), nx);
  double link_s = s_st, best = 1e30;
  for (const auto& l : A.apron_links)
    if (std::fabs(along(l.first) - s_st) < best) {
      best = std::fabs(along(l.first) - s_st);
      link_s = along(l.first);
    }
  std::vector<V3> p = {a.pos, at(exit_s - 30.0, 0.0), at(exit_s, 0.0), at(exit_s, tw), at(link_s, tw), at(link_s, lane_n), at(s_st, lane_n),
                       {st.pos.x, st.pos.y, st.pos.z + kJetRefZ}};
  p[0].z = A.rwy_a.z + kJetRefZ;
  setPath(a, fillet(p, 35.0));
}

namespace {
constexpr double kApproachDist = 12000.0;  // final approach from about 2,100 ft on a 3-degree path
constexpr double kCruiseAgl = 1800.0;      // short hop: cruise about 6,000 ft
}  // namespace

void Aviation::buildClimb(Airliner& a) const {
  // straight out climbing ~7 deg, a turn towards the destination's approach fix, cruise, then a
  // descent reaching the fix at the start of the final approach
  const Airport& D = airports_[static_cast<size_t>(a.to)];
  const Airport& O = airports_[static_cast<size_t>(a.from)];
  const V3 dux = norm(sub(D.rwy_b, D.rwy_a));
  const double fix_z = D.rwy_a.z + 15.0 + kApproachDist * std::tan(3.0 * kDeg) + kJetRefZ;
  const V3 fix{D.rwy_a.x - dux.x * kApproachDist, D.rwy_a.y - dux.y * kApproachDist, fix_z};
  const double cruise = std::max(O.terminal.z, D.terminal.z) + kCruiseAgl;
  const V3 start = a.pos;
  double hd = a.yaw;
  std::vector<V3> p = {start};
  V3 q = start;
  auto fwd = [&](double dist, double climb) {
    const int n = std::max(1, static_cast<int>(dist / 200.0));
    for (int i = 0; i < n; ++i) {
      q = {q.x + std::sin(hd) * dist / n, q.y + std::cos(hd) * dist / n, std::min(q.z + dist / n * std::tan(climb), cruise)};
      p.push_back(q);
    }
  };
  fwd(4500.0, 7.0 * kDeg);
  // turn (radius 3.5 km) until heading for the fix
  const double R = 3500.0;
  for (int i = 0; i < 72; ++i) {
    const double want = std::atan2(fix.x - q.x, fix.y - q.y);
    const double d = wrap(want - hd);
    if (std::fabs(d) < 3.0 * kDeg) break;
    const double step = std::clamp(d, -5.0 * kDeg, 5.0 * kDeg);
    const double arc = R * std::fabs(step);
    hd += step * 0.5;
    q = {q.x + std::sin(hd) * arc, q.y + std::cos(hd) * arc, std::min(q.z + arc * std::tan(5.0 * kDeg), cruise)};
    hd += step * 0.5;
    p.push_back(q);
  }
  // cruise and descent (about 3 degrees) to the fix, then join the final approach track
  const double dist = std::hypot(fix.x - q.x, fix.y - q.y);
  const int n = std::max(2, static_cast<int>(dist / 400.0));
  const V3 q0 = q;
  for (int i = 1; i <= n; ++i) {
    const double t = static_cast<double>(i) / n;
    const double remain = dist * (1.0 - t);
    V3 r{q0.x + (fix.x - q0.x) * t, q0.y + (fix.y - q0.y) * t, 0.0};
    const double climb_z = std::min(cruise, q0.z + dist * t * std::tan(4.0 * kDeg));
    r.z = std::max(fix.z, std::min(climb_z, fix.z + remain * std::tan(3.0 * kDeg)));
    p.push_back(r);
  }
  setPath(a, fillet(p, 2500.0));
}

void Aviation::buildApproach(Airliner& a) const {
  // 3-degree glide path onto threshold A of the destination, flare, touchdown ~350 m in, landing roll
  const Airport& A = airports_[static_cast<size_t>(a.to)];
  const V3 ux = norm(sub(A.rwy_b, A.rwy_a));
  const double L = len(sub(A.rwy_b, A.rwy_a));
  const double tz = A.rwy_a.z;
  auto rw = [&](double s, double h) { return V3{A.rwy_a.x + ux.x * s, A.rwy_a.y + ux.y * s, tz + h}; };
  std::vector<V3> p;
  p.push_back(a.pos);
  for (double s = -kApproachDist + 1500.0; s < 0.0; s += 1500.0) p.push_back(rw(s, 15.0 + (-s) * std::tan(3.0 * kDeg) + kJetRefZ));
  p.push_back(rw(0.0, 15.0 + kJetRefZ));
  p.push_back(rw(200.0, 5.0 + kJetRefZ));
  p.push_back(rw(330.0, 1.0 + kJetRefZ));
  p.push_back(rw(380.0, kJetRefZ));
  p.push_back(rw(L - 60.0, kJetRefZ));
  setPath(a, p);
}

void Aviation::refreshHeights(const World& world) {
  // the airports may be far from where the game started: take the ground heights once loaded
  bool all = true;
  for (Airport& A : airports_) {
    bool ok = true;
    auto fix = [&](rj::geo::Vec3d& p) {
      if (auto h = world.terrainHeight(p.x, p.y)) p.z = *h;
      else ok = false;
    };
    fix(A.rwy_a);
    fix(A.rwy_b);
    fix(A.twy_a);
    fix(A.twy_b);
    fix(A.lane_a);
    fix(A.lane_b);
    fix(A.terminal);
    fix(A.ga_stand);
    for (auto& c : A.connectors) fix(c.first), fix(c.second);
    for (auto& c : A.apron_links) fix(c.first), fix(c.second);
    for (auto& s : A.stands) fix(s.pos);
    if (ok)
      for (auto& a : jets_)
        if (a.phase == Airliner::Phase::AtStand && &airports_[static_cast<size_t>(a.from)] == &A)
          a.pos.z = A.stands[static_cast<size_t>(a.stand)].pos.z + kJetRefZ;
    all = all && ok;
  }
  heights_ok_ = all;
}

void Aviation::update(double dt, const World& world) {
  if (!placed_) return;
  if (!heights_ok_ && (height_retry_ -= dt) <= 0) {
    height_retry_ = 1.0;
    refreshHeights(world);
  }
  dt = std::min(dt, 0.1);
  for (auto& a : jets_) {
    const Airport& A = airports_[static_cast<size_t>(a.from)];  // departure airport (until landing)
    const Airport& D = airports_[static_cast<size_t>(a.to)];
    const V3 ux = norm(sub(A.rwy_b, A.rwy_a));
    const V3 dux = norm(sub(D.rwy_b, D.rwy_a));
    const float yaw0 = a.yaw;
    double hd = a.yaw, grade = 0;
    V3 p = a.pos;
    auto follow = [&](double vt, double acc, double dec) {
      const double remain = a.cum.back() - a.s;
      const double vstop = std::sqrt(2.0 * dec * std::max(0.0, remain));
      const double want = std::min(vt, vstop + 0.3);
      a.v = a.v < want ? std::min(want, a.v + acc * dt) : std::max(want, a.v - dec * dt);
      a.s = std::min(a.cum.back(), a.s + a.v * dt);
      pointAt(a, a.s, p, hd, grade);
      return remain;
    };
    switch (a.phase) {
      case Airliner::Phase::AtStand:
        a.timer -= dt;
        a.gear = 1;
        a.flaps = 0;
        if (a.timer <= 0) a.timer = 0;
        if (a.timer <= 0 && a.player_aboard && !a.player_seated) break;  // the crew waits for the passenger to sit down
        if (a.timer <= 0) {
          // pushback: straight back onto the apron taxilane
          const auto& st = A.stands[static_cast<size_t>(a.stand)];
          const V3 nx = norm(sub(A.twy_a, A.rwy_a));
          const double lane_n = dot(sub(A.lane_a, A.rwy_a), nx);
          const double st_n = dot(sub(st.pos, A.rwy_a), nx);
          V3 e = sub(st.pos, mul(nx, st_n - lane_n));
          e.z = st.pos.z + kJetRefZ;
          setPath(a, {a.pos, e});
          a.phase = Airliner::Phase::Pushback;
          a.v = 0;
        }
        break;
      case Airliner::Phase::Pushback: {
        const double remain = a.cum.back() - a.s;
        a.v = std::min(1.4, std::sqrt(2.0 * 0.3 * std::max(0.0, remain)) + 0.05);
        a.s = std::min(a.cum.back(), a.s + a.v * dt);
        pointAt(a, a.s, p, hd, grade);
        hd = a.yaw;  // tail first
        if (remain < 0.2) {
          buildTaxiOut(a);
          a.phase = Airliner::Phase::TaxiOut;
          a.v = 0;
          a.timer = 0;
        }
        break;
      }
      case Airliner::Phase::TaxiOut:
      case Airliner::Phase::TaxiIn: {
        // align on the spot first (after pushback), then taxi; slower through the turns
        V3 q;
        double h2, g2;
        pointAt(a, a.s + 3.0, q, h2, g2);
        const double d = wrap(h2 - a.yaw);
        if (a.v < 0.5 && std::fabs(d) > 8.0 * kDeg && a.s < 5.0) {
          hd = a.yaw + std::clamp(d, -6.0 * kDeg * dt, 6.0 * kDeg * dt);
          break;
        }
        V3 q2;
        pointAt(a, a.s + 45.0, q2, h2, g2);
        const double turn = std::fabs(wrap(h2 - a.yaw));
        const double vt = turn > 20.0 * kDeg ? 4.5 : 9.0;
        const double remain = follow(vt, 0.7, 0.9);
        hd = a.yaw + std::clamp(wrap(hd - a.yaw), -12.0 * kDeg * dt, 12.0 * kDeg * dt);
        if (remain < 0.3) {
          a.v = 0;
          if (a.phase == Airliner::Phase::TaxiOut) {
            // lined up: take-off roll along the runway
            const double L = len(sub(A.rwy_b, A.rwy_a));
            V3 end = {A.rwy_a.x + ux.x * (L - 50.0), A.rwy_a.y + ux.y * (L - 50.0), a.pos.z};
            V3 beyond = {A.rwy_a.x + ux.x * (L + 2500.0), A.rwy_a.y + ux.y * (L + 2500.0), a.pos.z + 2500.0 * std::tan(7.0 * kDeg)};
            setPath(a, {a.pos, end, beyond});
            a.phase = Airliner::Phase::Takeoff;
            a.timer = 6.0;  // hold on the runway briefly
          } else {
            // arrived: this aircraft now stands at the destination; the next leg goes back
            std::swap(a.from, a.to);
            a.phase = Airliner::Phase::AtStand;
            a.timer = 300.0;
            hd = airports_[static_cast<size_t>(a.from)].stands[static_cast<size_t>(a.stand)].heading * kDeg;
          }
        }
        break;
      }
      case Airliner::Phase::Takeoff: {
        if (a.timer > 0) {
          a.timer -= dt;
          a.flaps = 0.4f;
          break;
        }
        a.v += 2.0 * dt;
        a.s += a.v * dt;
        pointAt(a, a.s, p, hd, grade);
        // on the runway until lift-off at 76 m/s (about 150 kt)
        const double L = len(sub(A.rwy_b, A.rwy_a));
        const double s_rw = dot(sub(p, A.rwy_a), ux);
        if (a.v < 76.0 && s_rw < L - 60.0) p.z = a.pos.z;
        else {
          a.pos = p;
          buildClimb(a);
          a.phase = Airliner::Phase::Climb;
          a.timer = 0;
        }
        break;
      }
      case Airliner::Phase::Climb: {
        a.timer += dt;
        // initial climb about 165 kt, faster above 3,000 ft; slowing to 180 kt before the approach
        const double remain = a.cum.back() - a.s;
        const double vt = remain < 6000.0 ? 90.0 : (a.pos.z - A.terminal.z < 900.0 ? 85.0 : 125.0);
        a.v = a.v < vt ? std::min(vt, a.v + 1.1 * dt) : std::max(vt, a.v - 0.8 * dt);
        a.s += a.v * dt;
        pointAt(a, a.s, p, hd, grade);
        if (a.timer > 7.0 && remain > 9000.0) a.gear = std::max(0.0f, a.gear - static_cast<float>(dt) / 8.0f);
        if (a.v > 95.0) a.flaps = std::max(0.0f, a.flaps - static_cast<float>(dt) / 12.0f);
        if (remain < 5000.0) a.flaps = std::min(0.5f, a.flaps + static_cast<float>(dt) / 20.0f);
        hd = a.yaw + std::clamp(wrap(hd - a.yaw), -3.0 * kDeg * dt, 3.0 * kDeg * dt);
        if (a.s >= a.cum.back() - 1.0) {
          a.pos = p;
          buildApproach(a);
          a.phase = Airliner::Phase::Approach;
          a.gear = 1;
          a.flaps = 1;
        }
        break;
      }
      case Airliner::Phase::Offmap:  // (not used in the country: every flight lands on the map)
        a.phase = Airliner::Phase::Approach;
        break;
      case Airliner::Phase::Approach: {
        a.v = std::max(70.0, a.v - 0.5 * dt);
        a.s += a.v * dt;
        pointAt(a, a.s, p, hd, grade);
        hd = a.yaw + std::clamp(wrap(hd - a.yaw), -3.0 * kDeg * dt, 3.0 * kDeg * dt);
        const double s_rw = dot(sub(p, D.rwy_a), dux);
        if (s_rw >= 380.0) a.phase = Airliner::Phase::Landing;
        break;
      }
      case Airliner::Phase::Landing: {
        a.v = std::max(11.0, a.v - 2.7 * dt);  // reversers + brakes
        a.s = std::min(a.cum.back(), a.s + a.v * dt);
        pointAt(a, a.s, p, hd, grade);
        if (a.v <= 11.5) {
          a.pos = p;
          buildTaxiIn(a, dot(sub(p, D.rwy_a), dux));
          a.phase = Airliner::Phase::TaxiIn;
          a.flaps = 0;
        }
        break;
      }
    }
    // attitude: path grade plus angle of attack, bank from the turn rate
    const bool airborne = a.phase == Airliner::Phase::Climb || a.phase == Airliner::Phase::Approach;
    double pitch = std::atan(grade);
    if (a.phase == Airliner::Phase::Climb) pitch += (grade > 0.02 ? 5.0 : 2.0) * kDeg;
    if (a.phase == Airliner::Phase::Approach) pitch += 2.5 * kDeg;
    if (a.phase == Airliner::Phase::Takeoff && a.v > 68.0) pitch = std::min(8.0, (a.v - 68.0) * 1.2) * kDeg;
    a.pitch += static_cast<float>((pitch - a.pitch) * std::min(1.0, dt * 1.5));
    a.yaw = static_cast<float>(hd);
    const double yaw_rate = dt > 0 ? wrap(a.yaw - yaw0) / dt : 0.0;
    const double bank = airborne ? std::clamp(std::atan(a.v * yaw_rate / kG), -28.0 * kDeg, 28.0 * kDeg) : 0.0;
    a.roll += static_cast<float>((bank - a.roll) * std::min(1.0, dt * 1.2));
    a.pos = p;
    a.lights = true;
  }
}

const Airliner* Aviation::airliner(int id) const {
  for (const auto& a : jets_)
    if (a.id == id) return &a;
  return nullptr;
}

const Airliner* Aviation::boardable(int airport) const {
  for (const auto& a : jets_)
    if (a.phase == Airliner::Phase::AtStand && a.from == airport && a.timer > 15.0) return &a;
  return nullptr;
}

void Aviation::setAboard(int id, bool on) {
  for (auto& a : jets_)
    if (a.id == id) a.player_aboard = on;
}

void Aviation::setSeated(int id, bool on) {
  for (auto& a : jets_)
    if (a.id == id) a.player_seated = on;
}

void Aviation::fastForwardOffmap(int) {}

// ------------------------------------------------------------------------------------------------
// Light aircraft
namespace {
constexpr double kMass = 1050, kS = 16.2, kSpan = 11.0, kChord = 1.49;
constexpr double kIx = 1285, kIy = 1825, kIz = 2667;
struct Gear {
  double f, r, u;
  bool nose;
};
constexpr Gear kGear[3] = {{1.35, 0.0, -1.05, true}, {-0.45, 1.25, -1.05, false}, {-0.45, -1.25, -1.05, false}};
double seaZ(const World& world, const V3& p) {
  const auto g = world.toGeodetic(p);
  return world.toLocal({g.lat_deg, g.lon_deg, 0.0}).z;
}
}  // namespace

void LightPlane::reset(const V3& pos, double heading_deg, const World& world) {
  const double h = heading_deg * kDeg;
  f_ = {std::sin(h), std::cos(h), 0};
  u_ = {0, 0, 1};
  r_ = cross(f_, u_);
  pos_ = pos;
  snapped_ = false;
  if (auto gz = world.terrainHeight(pos.x, pos.y)) {
    pos_.z = *gz + 1.05;
    snapped_ = true;
  }
  vel_ = {0, 0, 0};
  p_ = q_ = yr_ = 0;
  ctl_ = cmd_ = PlaneControls{};
  ctl_.brake = true;
  crashed_ = false;
  on_ground_ = true;
  cam_init_ = false;
}

double LightPlane::heading() const {
  double h = std::atan2(f_.x, f_.y) / kDeg;
  return h < 0 ? h + 360.0 : h;
}
double LightPlane::pitchDeg() const { return std::asin(std::clamp(f_.z, -1.0, 1.0)) / kDeg; }
double LightPlane::rollDeg() const { return std::atan2(-r_.z, u_.z) / kDeg; }

void LightPlane::step(double h, const World& world, float wind_ms) {
  // air-relative velocity: a steady wind from the west (game assumption) and gusts about it, a random
  // walk that grows with the wind (turbulence; weaker near the ground)
  auto rnd = [&]() {
    grng_ ^= grng_ << 13;
    grng_ ^= grng_ >> 17;
    grng_ ^= grng_ << 5;
    return static_cast<double>(grng_ >> 8) / 8388608.0 - 1.0;
  };
  const double gs = (0.3 * wind_ms + 0.4) * 2.0 * std::sqrt(h);
  gust_.x += -1.5 * gust_.x * h + gs * rnd();
  gust_.y += -1.5 * gust_.y * h + gs * rnd();
  gust_.z += -2.0 * gust_.z * h + gs * 0.5 * rnd();
  const double gk = on_ground_ ? 0.2 : 1.0;
  const V3 wind{static_cast<double>(wind_ms) * 0.6 + gust_.x * gk, gust_.y * gk, gust_.z * gk};
  const V3 va = sub(vel_, wind);
  const double V = len(va);
  airspeed_ = V;
  const double vf = dot(va, f_), vr = dot(va, r_), vu = dot(va, u_);
  alpha_ = std::atan2(-vu, std::max(1.0, std::fabs(vf)));
  const double beta = V > 1.0 ? std::atan2(vr, std::max(1.0, std::fabs(vf))) : 0.0;
  const double qbar = 0.5 * 1.225 * V * V;
  const int fl = ctl_.flaps;
  // lift with stall
  const double a_stall = (16.0 - fl * 1.0) * kDeg;
  const double cl_lin = 0.25 + 4.8 * alpha_ + 0.18 * fl;
  const double cl_max = 0.25 + 4.8 * a_stall + 0.18 * fl;
  double cl = cl_lin;
  stall_ = alpha_ > a_stall && V > 8.0;
  if (alpha_ > a_stall) cl = std::max(cl_max * 0.6, cl_max - 2.5 * (alpha_ - a_stall));
  if (alpha_ < -a_stall) cl = std::min(-0.6, cl);
  const double cd = 0.032 + 0.045 * cl * cl + 0.012 * fl + (stall_ ? 0.15 : 0.0);
  V3 F{0, 0, -kMass * kG};
  if (V > 0.5) {
    const V3 vh = mul(va, 1.0 / V);
    const V3 ldir = norm(cross(r_, vh));
    F = add(F, mul(ldir, qbar * kS * cl));
    F = add(F, mul(vh, -qbar * kS * cd));
    F = add(F, mul(r_, qbar * kS * (-0.3 * beta)));
  }
  // propeller thrust falls off with airspeed
  const double thrust = ctl_.throttle * 2900.0 * std::max(0.0, 1.0 - std::max(0.0, vf) / 78.0);
  F = add(F, mul(f_, thrust));
  // aerodynamic moments (coefficients per rad, textbook light-aircraft values)
  const double Ve = std::max(V, 6.0);
  const double ph = p_ * kSpan / (2 * Ve), qh = q_ * kChord / (2 * Ve), rh = yr_ * kSpan / (2 * Ve);
  // elevator: Cm_de ~1.3 /rad with ~28 deg of travel -> about 0.6 at full deflection
  const double Cm = 0.02 - 1.0 * alpha_ + 0.62 * ctl_.elevator - 12.0 * qh - 0.03 * fl;
  const double Cl = -0.09 * beta + 0.18 * 0.35 * ctl_.aileron - 0.47 * ph + 0.1 * rh;
  const double Cn = 0.065 * beta + 0.07 * 0.35 * ctl_.rudder - 0.012 * 0.35 * ctl_.aileron - 0.1 * rh;
  double Mx = Cl * qbar * kS * kSpan, My = Cm * qbar * kS * kChord, Mz = Cn * qbar * kS * kSpan;
  Mz += -35.0 * ctl_.throttle * (1.0 - std::min(1.0, V / 40.0));  // propeller torque / p-factor: needs right rudder
  // landing gear
  on_ground_ = false;
  const V3 omega = sub(add(mul(f_, p_), mul(r_, q_)), mul(u_, yr_));
  const double gz_here = world.terrainHeight(pos_.x, pos_.y).value_or(-1e9);
  for (const auto& g : kGear) {
    const V3 off = add(add(mul(f_, g.f), mul(r_, g.r)), mul(u_, g.u));
    const V3 P = add(pos_, off);
    const double gz = world.terrainHeight(P.x, P.y).value_or(gz_here);
    const double hgt = P.z - gz;
    if (hgt >= 0) continue;
    on_ground_ = true;
    const V3 vp = add(vel_, cross(omega, off));
    if (vp.z < -5.5) crashed_ = true;  // hard landing
    const double N = std::max(0.0, 42000.0 * (-hgt) - 4200.0 * vp.z);
    // wheel heading: nose wheel steers with the rudder at low speed
    const double steer = g.nose ? 0.35 * ctl_.rudder * std::clamp(1.0 - V / 25.0, 0.0, 1.0) : 0.0;
    const double fh = std::atan2(f_.x, f_.y) + steer;
    const V3 wf{std::sin(fh), std::cos(fh), 0}, wr{std::cos(fh), -std::sin(fh), 0};
    const double vlong = dot(vp, wf), vlat = dot(vp, wr);
    const double mu_lat = 0.8 * N;
    double flong = -std::clamp(vlong * 400.0, -0.025 * N, 0.025 * N);  // rolling resistance
    if (!g.nose && ctl_.brake) flong = -std::clamp(vlong * 3000.0, -0.55 * N, 0.55 * N);
    const double flat = -std::clamp(vlat * 3000.0, -mu_lat, mu_lat);
    const V3 Fg = add(add(V3{0, 0, N}, mul(wf, flong)), mul(wr, flat));
    F = add(F, Fg);
    const V3 tau = cross(off, Fg);
    Mx += dot(tau, f_);
    My += dot(tau, r_);
    Mz += -dot(tau, u_);
  }
  // other parts touching the ground: propeller, wing tips, tail -> crash
  const double probes[4][3] = {{2.1, 0, -0.35}, {0, 5.5, 1.05}, {0, -5.5, 1.05}, {-5.8, 0, 0.3}};
  for (const auto& pr : probes) {
    const V3 P = add(pos_, add(add(mul(f_, pr[0]), mul(r_, pr[1])), mul(u_, pr[2])));
    const double gz = world.terrainHeight(P.x, P.y).value_or(gz_here);
    if (P.z < gz - 0.05 && V > 3.0) crashed_ = true;
  }
  // integrate
  vel_ = add(vel_, mul(F, h / kMass));
  pos_ = add(pos_, mul(vel_, h));
  p_ += Mx / kIx * h;
  q_ += My / kIy * h;
  yr_ += Mz / kIz * h;
  if (on_ground_) {  // tyre scrub keeps a parked aeroplane from creeping round
    p_ *= 1.0 - std::min(1.0, 8.0 * h);
    if (V < 1.0) yr_ *= 1.0 - std::min(1.0, 10.0 * h);
  }
  const V3 w = sub(add(mul(f_, p_), mul(r_, q_)), mul(u_, yr_));
  f_ = add(f_, mul(cross(w, f_), h));
  r_ = add(r_, mul(cross(w, r_), h));
  f_ = norm(f_);
  r_ = norm(sub(r_, mul(f_, dot(r_, f_))));
  u_ = cross(r_, f_);
  // water and buildings
  if (pos_.z < seaZ(world, pos_) + 0.3 && !on_ground_) crashed_ = true;
  V3 c = pos_;
  world.collide(c, 2.0);
  if (std::hypot(c.x - pos_.x, c.y - pos_.y) > 0.05) crashed_ = true;
}

void LightPlane::update(double dt, const World& world, const PlaneControls& in, float wind_ms) {
  if (crashed_) return;
  if (!snapped_) {  // parked: wait for the ground under the wheels to stream in
    if (auto gz = world.terrainHeight(pos_.x, pos_.y)) {
      pos_.z = *gz + 1.05;
      snapped_ = true;
    } else {
      return;
    }
  }
  dt = std::min(dt, 0.05);
  // control surfaces move at a finite rate; throttle and flaps as commanded
  auto slew = [&](float& c, float t, float r) { c += std::clamp(t - c, -r * static_cast<float>(dt), r * static_cast<float>(dt)); };
  slew(ctl_.elevator, in.elevator, 3.0f);
  slew(ctl_.aileron, in.aileron, 4.0f);
  slew(ctl_.rudder, in.rudder, 3.0f);
  ctl_.throttle = std::clamp(in.throttle, 0.0f, 1.0f);
  ctl_.flaps = std::clamp(in.flaps, 0, 3);
  ctl_.brake = in.brake;
  const int n = std::max(1, static_cast<int>(std::ceil(dt / (1.0 / 200.0))));
  for (int i = 0; i < n && !crashed_; ++i) step(dt / n, world, wind_ms);
  rpm_ = static_cast<float>(std::clamp(650.0 + ctl_.throttle * 1750.0 + std::max(0.0, dot(vel_, f_)) * 7.0, 0.0, 2750.0));
  prop_angle_ = std::fmod(prop_angle_ + rpm_ / 60.0f * 2.0f * PI * static_cast<float>(dt), 2.0f * PI);
  // chase camera follows smoothly
  const V3 back = sub(pos_, mul(len(vel_) > 5.0 ? norm(vel_) : f_, 14.0));
  const V3 want{back.x, back.y, back.z + 3.5};
  if (!cam_init_) {
    cam_pos_ = want;
    cam_init_ = true;
  }
  const double k = std::min(1.0, dt * 4.0);
  cam_pos_ = add(cam_pos_, mul(sub(want, cam_pos_), k));
}

Matrix LightPlane::modelMatrix() const {
  const Vector3 p = enuToRl(pos_);
  auto d = [](const V3& v) { return Vector3{static_cast<float>(v.x), static_cast<float>(v.z), static_cast<float>(-v.y)}; };
  const Vector3 X = d(r_), Y = d(u_), Z = d(mul(f_, -1.0));
  Matrix m = MatrixIdentity();
  m.m0 = X.x, m.m1 = X.y, m.m2 = X.z;
  m.m4 = Y.x, m.m5 = Y.y, m.m6 = Y.z;
  m.m8 = Z.x, m.m9 = Z.y, m.m10 = Z.z;
  m.m12 = p.x, m.m13 = p.y, m.m14 = p.z;
  return m;
}

Camera3D LightPlane::camera(float fov, bool cockpit, float look_yaw, float look_pitch) const {
  Camera3D c{};
  c.fovy = fov;
  c.projection = CAMERA_PERSPECTIVE;
  if (cockpit) {
    // left seat, eye under the wing root
    const V3 eye = add(add(add(pos_, mul(f_, 0.35)), mul(r_, -0.3)), mul(u_, 0.3));
    const V3 dir = add(add(mul(f_, std::cos(look_yaw) * std::cos(look_pitch)), mul(r_, std::sin(look_yaw) * std::cos(look_pitch))),
                       mul(u_, std::sin(look_pitch)));
    c.position = enuToRl(eye);
    c.target = enuToRl(add(eye, dir));
    const V3 up = u_;
    c.up = {static_cast<float>(up.x), static_cast<float>(up.z), static_cast<float>(-up.y)};
  } else {
    const double y = std::atan2(f_.x, f_.y) + look_yaw;
    V3 eye = cam_pos_;
    if (std::fabs(look_yaw) > 0.05) {  // orbit with the mouse
      const double d = 14.0;
      eye = {pos_.x - std::sin(y) * d, pos_.y - std::cos(y) * d, pos_.z + 3.5 + std::sin(look_pitch) * 8.0};
    }
    c.position = enuToRl(eye);
    c.target = enuToRl({pos_.x, pos_.y, pos_.z + 1.0});
    c.up = {0, 1, 0};
  }
  return c;
}

}  // namespace rjc
