#include "game/ferries.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "platform/paths.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
double wrap(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}
// Generic ships (no real vessel): a 62 m island-hopping ferry and a 110 m car ferry.
const ShipClass kClasses[2] = {
    {62.0f, 13.0f, 3.2f, 6.1f, 5.7f, -26.0f, 14.0f, 4.2f, -14.0f, 8.0f},
    {110.0f, 20.0f, 5.5f, 11.3f, 9.2f, -44.0f, 26.0f, 7.0f, -28.0f, 16.0f},
};
double cruiseOf(int cls) { return cls == 0 ? 8.5 : 10.5; }  // 16.5 / 20 knots
}  // namespace

const ShipClass& Ferries::shipClass(int cls) { return kClasses[std::clamp(cls, 0, 1)]; }

bool Ferries::load(const std::filesystem::path& transport, std::string& err) {
  auto text = readText(transport);
  if (!text) {
    err = "no transport.txt";
    return false;
  }
  std::istringstream in(*text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "pier") {
      Pier p;
      ls >> p.geo.lat_deg >> p.geo.lon_deg >> p.heading;
      p.geo.h_ellipsoidal_m = 0;
      piers_.push_back(p);
    } else if (k == "ferry") {
      int idx;
      ls >> idx;
      Route r;
      std::string tok;
      while (ls >> tok) {
        rj::geo::Geodetic g{};
        if (std::sscanf(tok.c_str(), "%lf,%lf", &g.lat_deg, &g.lon_deg) == 2) r.via.push_back(g);
      }
      if (r.via.size() >= 2) routes_.push_back(r);
    }
  }
  if (piers_.empty() || routes_.empty()) {
    err = "transport.txt has no piers / ferry routes";
    return false;
  }
  return true;
}

void Ferries::place(const World& world) {
  for (auto& p : piers_) p.pos = world.toLocal(p.geo);
  auto nearestPier = [&](const rj::geo::Vec3d& q, double maxd) {
    int best = -1;
    double bd = maxd;
    for (int i = 0; i < static_cast<int>(piers_.size()); ++i) {
      const auto& P = piers_[static_cast<size_t>(i)];
      const double th = P.heading * kDeg;
      const rj::geo::Vec3d head{P.pos.x + std::sin(th) * P.length, P.pos.y + std::cos(th) * P.length, 0};
      const double d = std::min(std::hypot(q.x - P.pos.x, q.y - P.pos.y), std::hypot(q.x - head.x, q.y - head.y));
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
    return best;
  };
  const bool first = !placed_;
  if (first) {
    int id = 1;
    for (int r = 0; r < static_cast<int>(routes_.size()) && r < 2; ++r) {
      Ferry f;
      f.id = id++;
      f.route = r;
      f.cls = r == 0 ? 0 : 1;
      ships_.push_back(f);
    }
  }
  for (auto& f : ships_) {
    const Route& R = routes_[static_cast<size_t>(f.route)];
    std::vector<rj::geo::Vec3d> via;
    for (const auto& g : R.via) via.push_back(world.toLocal(g));
    f.pier_a = nearestPier(via.front(), 400.0);
    f.pier_b = nearestPier(via.back(), 400.0);
    if (f.pier_b == f.pier_a) f.pier_b = -1;
    if (f.pier_a < 0) continue;
    const ShipClass& C = shipClass(f.cls);
    auto berthAt = [&](int pier, int side, rj::geo::Vec3d& at, rj::geo::Vec3d& out) {
      const Pier& P = piers_[static_cast<size_t>(pier)];
      const double th = P.heading * kDeg;
      const double fx = std::sin(th), fy = std::cos(th), rx = std::cos(th), ry = -std::sin(th);
      const double along = f.cls == 0 ? 0.58 : 0.62, off = 6.0 + C.beam * 0.5 + 1.2;
      at = {P.pos.x + fx * P.length * along + rx * side * off, P.pos.y + fy * P.length * along + ry * side * off, P.pos.z};
      out = {at.x + fx * (P.length * (1.0 - along) + C.length * 0.5 + 120.0), at.y + fy * (P.length * (1.0 - along) + C.length * 0.5 + 120.0), P.pos.z};
    };
    f.path.clear();
    rj::geo::Vec3d atA, outA;
    berthAt(f.pier_a, f.route == 0 ? 1 : -1, atA, outA);
    f.path.push_back(atA);
    f.path.push_back(outA);
    for (size_t i = 1; i + 1 < via.size(); ++i) f.path.push_back({via[i].x, via[i].y, atA.z});
    if (f.pier_b >= 0) {
      rj::geo::Vec3d atB, outB;
      berthAt(f.pier_b, -1, atB, outB);
      f.path.push_back(outB);
      f.path.push_back(atB);
    } else {  // towards the mainland: continue well beyond the last point, out of sight
      const auto& a = via[via.size() - 2];
      const auto& b = via.back();
      const double dx = b.x - a.x, dy = b.y - a.y, l = std::max(1.0, std::hypot(dx, dy));
      f.path.push_back({b.x, b.y, atA.z});
      f.path.push_back({b.x + dx / l * 4000.0, b.y + dy / l * 4000.0, atA.z});
    }
    f.cum.assign(f.path.size(), 0.0);
    for (size_t i = 1; i < f.path.size(); ++i)
      f.cum[i] = f.cum[i - 1] + std::hypot(f.path[i].x - f.path[i - 1].x, f.path[i].y - f.path[i - 1].y);
    f.length = f.cum.back();
    if (first) {
      f.s = 0;
      f.dir = 1;
      f.phase = Ferry::Phase::Docked;
      f.timer = f.cls == 0 ? 40.0 : 70.0;
      double h;
      pointAt(f, 1.0, f.pos, h);
      f.yaw = static_cast<float>(h + kPi);  // alongside, bow towards the land
    }
    f.trail.clear();
  }
  placed_ = true;
}

void Ferries::pointAt(const Ferry& f, double s, rj::geo::Vec3d& p, double& heading) const {
  s = std::clamp(s, 0.0, f.length);
  size_t k = static_cast<size_t>(std::upper_bound(f.cum.begin(), f.cum.end(), s) - f.cum.begin());
  k = std::clamp<size_t>(k, 1, f.path.size() - 1);
  const auto& A = f.path[k - 1];
  const auto& B = f.path[k];
  const double seg = std::max(1e-6, f.cum[k] - f.cum[k - 1]);
  const double t = std::clamp((s - f.cum[k - 1]) / seg, 0.0, 1.0);
  p = {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z};
  heading = std::atan2(B.x - A.x, B.y - A.y);
}

void Ferries::startTrip(Ferry& f) {
  // backing out of the berth first (the far end off the map is left bow first)
  const bool from_pier = f.dir > 0 || f.pier_b >= 0;
  f.astern_len = 0;
  if (from_pier) f.astern_len = f.dir > 0 ? f.cum[1] : f.length - f.cum[f.cum.size() - 2];
  f.phase = f.astern_len > 0 ? Ferry::Phase::Astern : Ferry::Phase::Turning;
  f.v = 0;
}

void Ferries::update(double dt, double t_s, float wind) {
  if (!placed_) return;
  dt = std::min(dt, 0.1);
  for (auto& f : ships_) {
    if (f.path.size() < 2) continue;
    const ShipClass& C = shipClass(f.cls);
    const double q = f.dir > 0 ? f.s : f.length - f.s;  // progress along this trip
    const double remain = f.length - q;
    auto advance = [&](double v) {
      f.s += f.dir * v * dt;
      f.s = std::clamp(f.s, 0.0, f.length);
    };
    double ph;  // path tangent in the travel direction
    rj::geo::Vec3d pp;
    pointAt(f, f.s + f.dir * 0.5, pp, ph);
    if (f.dir < 0) ph += kPi;
    switch (f.phase) {
      case Ferry::Phase::Docked:
        f.v = 0;
        f.timer -= dt;
        if (f.timer <= 0) startTrip(f);
        break;
      case Ferry::Phase::Astern: {
        // stern first along the first leg, bow still pointing at the land
        const double left = f.astern_len - q;
        const double vmax = std::min(1.6, std::sqrt(2.0 * 0.08 * std::max(0.0, left)));
        f.v = std::min(vmax, f.v + 0.06 * dt);
        advance(f.v);
        if (left < 0.3) {
          f.v = 0;
          f.phase = Ferry::Phase::Turning;
        }
        break;
      }
      case Ferry::Phase::Turning: {
        rj::geo::Vec3d a;
        double h;
        pointAt(f, f.s + f.dir * 60.0, a, h);
        const double want = std::atan2(a.x - f.pos.x, a.y - f.pos.y);
        const double d = wrap(want - f.yaw);
        const double rate = (f.cls == 0 ? 3.0 : 2.0) * kDeg;
        f.yaw = static_cast<float>(wrap(f.yaw + std::clamp(d, -rate * dt, rate * dt)));
        if (std::fabs(d) < 1.5 * kDeg) f.phase = Ferry::Phase::Under;
        break;
      }
      case Ferry::Phase::Under: {
        const bool pier_end = f.dir > 0 ? f.pier_b >= 0 : true;
        double vmax = cruiseOf(f.cls);
        if (pier_end) {
          vmax = std::min(vmax, 0.5 + std::sqrt(2.0 * 0.1 * std::max(0.0, remain - 0.5)));  // come alongside gently
          if (remain < 420.0) vmax = std::min(vmax, 3.0);
        }
        if (q < 200.0) vmax = std::min(vmax, 3.0);  // harbour speed
        const double acc = f.v < vmax ? 0.12 : -0.18;
        f.v = acc > 0 ? std::min(vmax, f.v + acc * dt) : std::max(vmax, f.v + acc * dt);
        advance(f.v);
        // heading follows the course ahead (turn rate limited)
        rj::geo::Vec3d a;
        double h;
        pointAt(f, f.s + f.dir * (f.cls == 0 ? 45.0 : 70.0), a, h);
        const double want = remain < 3.0 ? ph : std::atan2(a.x - f.pos.x, a.y - f.pos.y);
        const double rate = (f.cls == 0 ? 3.5 : 2.2) * kDeg;
        f.yaw = static_cast<float>(wrap(f.yaw + std::clamp(wrap(want - f.yaw), -rate * dt, rate * dt)));
        if (remain < 0.4) {
          f.v = 0;
          if (pier_end) {
            f.phase = Ferry::Phase::Docked;
            f.timer = f.cls == 0 ? 60.0 : 90.0;
          } else {
            f.phase = Ferry::Phase::Offmap;
            f.timer = 240.0;
          }
          f.dir = -f.dir;
        }
        break;
      }
      case Ferry::Phase::Offmap:
        f.timer -= dt;
        if (f.timer <= 0) {
          f.phase = Ferry::Phase::Under;
          f.yaw = static_cast<float>(ph);
          f.trail.clear();
        }
        break;
    }
    rj::geo::Vec3d p;
    double h;
    pointAt(f, f.s, p, h);
    f.pos = p;
    // swell: smaller ships move more; less alongside the pier
    const double calm = f.phase == Ferry::Phase::Docked ? 0.35 : 1.0;
    const double k = std::clamp(static_cast<double>(wind), 0.2, 1.6) * calm * (f.cls == 0 ? 1.0 : 0.55);
    f.heave = static_cast<float>(0.25 * k * std::sin(0.55 * t_s + f.id));
    f.pitch = static_cast<float>(0.9 * kDeg * k * std::sin(0.42 * t_s + 1.3 * f.id));
    f.roll = static_cast<float>(1.8 * kDeg * k * std::sin(0.31 * t_s + 0.7 * f.id));
    f.wake = static_cast<float>(std::clamp(std::fabs(f.v) / cruiseOf(f.cls), 0.0, 1.0));
    // wake trail from the stern
    const rj::geo::Vec3d stern{f.pos.x - std::sin(f.yaw) * C.length * 0.5, f.pos.y - std::cos(f.yaw) * C.length * 0.5, f.pos.z};
    if (f.phase == Ferry::Phase::Offmap) {
      f.trail.clear();
    } else if (f.trail.empty() || std::hypot(stern.x - f.trail.back().x, stern.y - f.trail.back().y) > 4.0) {
      f.trail.push_back(stern);
      if (f.trail.size() > 70) f.trail.erase(f.trail.begin());
    }
  }
}

const Ferry* Ferries::ship(int id) const {
  for (const auto& f : ships_)
    if (f.id == id) return &f;
  return nullptr;
}

const Ferry* Ferries::dockedAt(int pier) const {
  for (const auto& f : ships_)
    if (f.phase == Ferry::Phase::Docked && currentPier(f) == pier && f.timer > 5.0) return &f;
  return nullptr;
}

int Ferries::pierNear(const rj::geo::Vec3d& p, double r) const {
  int best = -1;
  double bd = r;
  for (int i = 0; i < static_cast<int>(piers_.size()); ++i) {
    const auto& P = piers_[static_cast<size_t>(i)];
    const double th = P.heading * kDeg;
    const double fx = std::sin(th), fy = std::cos(th);
    const double t = std::clamp((p.x - P.pos.x) * fx + (p.y - P.pos.y) * fy, -30.0, P.length);
    const double d = std::hypot(p.x - (P.pos.x + fx * t), p.y - (P.pos.y + fy * t));
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

rj::geo::Vec3d Ferries::gangwayPier(const Ferry& f) const {
  const int pi = currentPier(f) >= 0 ? currentPier(f) : f.pier_a;
  const Pier& P = piers_[static_cast<size_t>(std::max(0, pi))];
  const double th = P.heading * kDeg;
  const double fx = std::sin(th), fy = std::cos(th);
  const double t = std::clamp((f.pos.x - P.pos.x) * fx + (f.pos.y - P.pos.y) * fy, 5.0, P.length - 5.0);
  const rj::geo::Vec3d c{P.pos.x + fx * t, P.pos.y + fy * t, P.pos.z};
  const double dx = f.pos.x - c.x, dy = f.pos.y - c.y, d = std::max(1e-3, std::hypot(dx, dy));
  return {c.x + dx / d * 3.5, c.y + dy / d * 3.5, P.pos.z + 2.4};
}

rj::geo::Vec3d Ferries::toWorld(const Ferry& f, double x, double y, double z) const {
  const double fx = std::sin(f.yaw), fy = std::cos(f.yaw), rx = fy, ry = -fx;
  const double zz = z + f.heave + y * std::sin(f.pitch) - x * std::sin(f.roll);
  return {f.pos.x + rx * x + fx * y, f.pos.y + ry * x + fy * y, f.pos.z + zz};
}

void Ferries::toShip(const Ferry& f, const rj::geo::Vec3d& p, double& x, double& y) const {
  const double fx = std::sin(f.yaw), fy = std::cos(f.yaw), rx = fy, ry = -fx;
  const double dx = p.x - f.pos.x, dy = p.y - f.pos.y;
  x = dx * rx + dy * ry;
  y = dx * fx + dy * fy;
}

void Ferries::fastForwardOffmap(int id) {
  for (auto& f : ships_)
    if (f.id == id && f.phase == Ferry::Phase::Offmap) f.timer = std::min(f.timer, 4.0);
}

}  // namespace rjc
