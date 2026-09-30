#include "game/trains.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "game/station_names.hpp"
#include "platform/paths.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool Trains::load(const std::filesystem::path& file, std::string& err) {
  auto text = readText(file);
  if (!text) {
    err = "no rail.txt";
    return false;
  }
  std::istringstream in(*text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "line") {
      int idx = 0, n = 0;
      std::string kind;
      ls >> idx >> kind >> n;
      RailLine L;
      L.kind = kind == "shinkansen" ? LineKind::Shinkansen : kind == "branch" ? LineKind::Branch : LineKind::Loop;
      L.closed = L.kind == LineKind::Loop;
      std::string names;
      std::getline(ls, names);  // "<name>|<english>" (optional)
      names.erase(0, names.find_first_not_of(' '));
      const auto bar = names.find('|');
      L.name = names.substr(0, bar);
      if (bar != std::string::npos) L.name_en = names.substr(bar + 1);
      for (int i = 0; i < n && std::getline(in, line); ++i) {
        std::istringstream ps(line);
        double la, lo, z;
        ps >> la >> lo >> z;
        L.geo.push_back({la, lo, z});
      }
      lines_.push_back(std::move(L));
    } else if (k == "station") {
      Station st;
      double la, lo;
      ls >> st.line >> la >> lo >> st.heading >> st.ztop_h;
      std::getline(ls, st.name);
      st.name.erase(0, st.name.find_first_not_of(' '));
      st.geo = {la, lo, st.ztop_h};
      stations_.push_back(st);
    } else if (k == "gate") {
      StationGate g;
      double la, lo, z;
      ls >> g.station >> la >> lo >> z >> g.heading;
      g.geo = {la, lo, z};
      float v;
      while (ls >> v) g.lanes.push_back(v);
      if (!g.lanes.empty()) gates_.push_back(std::move(g));
    } else if (k == "crossing") {
      LevelCrossing c;
      double la, lo, z;
      ls >> c.id >> c.line >> la >> lo >> z >> c.road_hd >> c.half;
      c.geo = {la, lo, z};
      crossings_.push_back(c);
    } else if (k == "xset") {
      int id = 0;
      LevelCrossing::Set st;
      double la, lo, z;
      ls >> id >> la >> lo >> z >> st.facing >> st.arm_hd >> st.arm_len;
      st.geo = {la, lo, z};
      for (auto& c : crossings_)
        if (c.id == id) c.sets.push_back(st);
    } else if (k == "reading") {
      std::string name, kana, roman;
      ls >> name >> kana;
      std::getline(ls, roman);
      roman.erase(0, roman.find_first_not_of(' '));
      if (!name.empty() && !kana.empty()) addStationReading(name, kana, roman);
    }
  }
  if (lines_.empty()) {
    err = "rail.txt has no lines";
    return false;
  }
  return true;
}

void Trains::pointAt(const RailLine& L, double s, rj::geo::Vec3d& p, double& heading, double& grade) const {
  if (L.closed) {
    s = std::fmod(s, L.length);
    if (s < 0) s += L.length;
  } else {
    s = std::clamp(s, 0.0, L.length);
  }
  size_t k = static_cast<size_t>(std::upper_bound(L.cum.begin(), L.cum.end(), s) - L.cum.begin());
  k = std::clamp<size_t>(k, 1, L.pts.size() - 1);
  const auto& A = L.pts[k - 1];
  const auto& B = L.pts[k];
  const double seg = std::max(1e-6, L.cum[k] - L.cum[k - 1]);
  const double t = std::clamp((s - L.cum[k - 1]) / seg, 0.0, 1.0);
  p = {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z + (B.z - A.z) * t};
  heading = std::atan2(B.x - A.x, B.y - A.y);
  grade = (B.z - A.z) / seg;
}

double Trains::lateral(const Station& st, const rj::geo::Vec3d& p) const {
  if (st.line < 0 || st.line >= static_cast<int>(lines_.size())) return 0.0;
  const auto& L = lines_[static_cast<size_t>(st.line)];
  double best = 1e30, lat = 0.0;
  for (size_t i = 1; i < L.pts.size(); ++i) {
    double ds = std::fabs(L.cum[i] - st.s);
    if (L.closed) ds = std::min(ds, L.length - ds);
    if (ds > 260.0) continue;  // the stretch through this station
    const auto& A = L.pts[i - 1];
    const auto& B = L.pts[i];
    const double vx = B.x - A.x, vy = B.y - A.y, l2 = vx * vx + vy * vy;
    if (l2 <= 0) continue;
    const double t = std::clamp(((p.x - A.x) * vx + (p.y - A.y) * vy) / l2, 0.0, 1.0);
    const double qx = A.x + vx * t, qy = A.y + vy * t, d = std::hypot(p.x - qx, p.y - qy);
    if (d < best) {
      best = d;
      const double l = std::sqrt(l2);
      lat = ((p.x - A.x) * vy - (p.y - A.y) * vx) / l;  // right of the segment direction
    }
  }
  // the line's direction may run against the station heading
  const double hd = st.heading * 0.017453292519943295;  // degrees -> radians
  rj::geo::Vec3d q;
  double h, g;
  pointAt(L, st.s, q, h, g);
  return std::cos(h - hd) >= 0 ? lat : -lat;
}

void Trains::place(const World& world) {
  for (auto& L : lines_) {
    L.pts.clear();
    L.cum.clear();
    double acc = 0;
    for (const auto& g : L.geo) {
      const auto q = world.toLocal(g);
      if (!L.pts.empty()) acc += std::hypot(q.x - L.pts.back().x, q.y - L.pts.back().y);
      L.pts.push_back(q);
      L.cum.push_back(acc);
    }
    L.length = acc;
    // curve speed limits: radius from the heading change over +-30 m; a = 2.0 m/s^2 in total
    // (mostly balanced by the track's cant), capped by the line's top speed
    const size_t n = L.pts.size();
    const double vtop = L.kind == LineKind::Shinkansen ? 83.3 : L.kind == LineKind::Branch ? 33.3 : 25.0;
    L.vlim.assign(n, vtop);
    for (size_t i = 0; i < n; ++i) {
      size_t a = i, b = i;
      while (a > 0 && L.cum[i] - L.cum[a] < 30.0) --a;
      while (b + 1 < n && L.cum[b] - L.cum[i] < 30.0) ++b;
      if (a == i || b == i || a + 1 >= n || b < 1) continue;
      const double h0 = std::atan2(L.pts[a + 1].x - L.pts[a].x, L.pts[a + 1].y - L.pts[a].y);
      const double h1 = std::atan2(L.pts[b].x - L.pts[b - 1].x, L.pts[b].y - L.pts[b - 1].y);
      double dh = std::fabs(std::remainder(h1 - h0, 2.0 * kPi));
      const double ds = std::max(1.0, L.cum[b] - L.cum[a]);
      if (dh < 1e-4) continue;
      const double R = ds / dh;
      L.vlim[i] = std::min(vtop, std::max(8.0, std::sqrt(2.0 * R)));
    }
    const double brake = L.kind == LineKind::Shinkansen ? 0.8 : 1.0;
    L.env_fwd = L.vlim;
    L.env_bwd = L.vlim;
    for (size_t i = n - 1; i-- > 0;) {
      const double ds = L.cum[i + 1] - L.cum[i];
      L.env_fwd[i] = std::min(L.env_fwd[i], std::sqrt(L.env_fwd[i + 1] * L.env_fwd[i + 1] + 2.0 * brake * ds));
    }
    for (size_t i = 1; i < n; ++i) {
      const double ds = L.cum[i] - L.cum[i - 1];
      L.env_bwd[i] = std::min(L.env_bwd[i], std::sqrt(L.env_bwd[i - 1] * L.env_bwd[i - 1] + 2.0 * brake * ds));
    }
  }
  for (auto& g : gates_) g.pos = world.toLocal(g.geo);
  for (auto& c : crossings_) {
    c.pos = world.toLocal(c.geo);
    for (auto& st : c.sets) st.pos = world.toLocal(st.geo);
    if (c.line < 0 || c.line >= static_cast<int>(lines_.size())) continue;
    const auto& L = lines_[static_cast<size_t>(c.line)];
    double best = 1e30;
    for (size_t i = 1; i < L.pts.size(); ++i) {
      const auto& A = L.pts[i - 1];
      const auto& B = L.pts[i];
      const double vx = B.x - A.x, vy = B.y - A.y, l2 = vx * vx + vy * vy;
      const double t = l2 > 0 ? std::clamp(((c.pos.x - A.x) * vx + (c.pos.y - A.y) * vy) / l2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(c.pos.x - (A.x + t * vx), c.pos.y - (A.y + t * vy));
      if (d < best) {
        best = d;
        c.s = L.cum[i - 1] + t * std::sqrt(l2);
      }
    }
  }
  for (auto& st : stations_) {
    st.pos = world.toLocal(st.geo);
    if (st.line < 0 || st.line >= static_cast<int>(lines_.size())) continue;
    const auto& L = lines_[static_cast<size_t>(st.line)];
    double best = 1e30;
    for (size_t i = 1; i < L.pts.size(); ++i) {
      const auto& A = L.pts[i - 1];
      const auto& B = L.pts[i];
      const double vx = B.x - A.x, vy = B.y - A.y, l2 = vx * vx + vy * vy;
      const double t = l2 > 0 ? std::clamp(((st.pos.x - A.x) * vx + (st.pos.y - A.y) * vy) / l2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(st.pos.x - (A.x + t * vx), st.pos.y - (A.y + t * vy));
      if (d < best) {
        best = d;
        st.s = L.cum[i - 1] + t * std::sqrt(l2);
      }
    }
  }
  if (placed_) return;
  placed_ = true;
  int id = 1;
  for (int li = 0; li < static_cast<int>(lines_.size()); ++li) {
    const auto& L = lines_[static_cast<size_t>(li)];
    auto mk = [&](int dir, double s, int cars, double len, double vmax) {
      Train t;
      t.id = id++;
      t.line = li;
      t.dir = dir;
      t.s = s;
      t.cars = cars;
      t.car_len = len;
      t.vmax = vmax;
      chooseNextStop(t);
      trains_.push_back(t);
    };
    if (L.kind == LineKind::Loop) {
      for (int k = 0; k < 3; ++k) {
        mk(1, L.length * (k + 0.15) / 3.0, 10, 20.0, 25.0);
        mk(-1, L.length * (k + 0.65) / 3.0, 10, 20.0, 25.0);
      }
    } else {
      // trains spread along the line in both directions (about one every 12 km each way on the
      // main line, every 20 km on the Shinkansen), the first of each direction at its terminal
      const bool sk = L.kind == LineKind::Shinkansen;
      const int cars = sk ? 8 : 6;
      const double len = sk ? 25.0 : 20.0, vmax = sk ? 83.3 : 33.3;
      const int per_dir = std::max(1, static_cast<int>(L.length / (sk ? 20000.0 : 12000.0)));
      for (int k = 0; k < per_dir; ++k) {
        mk(1, 205.0 + (L.length - 400.0) * k / per_dir, cars, len, vmax);
        mk(-1, L.length - 10.0 - (L.length - 400.0) * k / per_dir, cars, len, vmax);
      }
    }
  }
  // The first train of each direction waits at its terminal with the doors open.
  for (auto& t : trains_) {
    const auto& L = lines_[static_cast<size_t>(t.line)];
    if (L.closed) continue;
    const bool first_of_dir = t.dir > 0 ? t.s < 300.0 : t.s > L.length - 300.0;
    if (!first_of_dir) continue;
    int term = -1;
    for (int i = 0; i < static_cast<int>(stations_.size()); ++i)
      if (stations_[static_cast<size_t>(i)].line == t.line &&
          (term < 0 || stations_[static_cast<size_t>(i)].s * t.dir < stations_[static_cast<size_t>(term)].s * t.dir))
        term = i;
    if (term < 0) continue;
    t.s = stopMark(t, term);
    t.at_station = term;
    t.dwell = 60.0;
    t.dwell0 = 63.0;  // already open at the start
    t.next_stop = term;
  }
}

double Trains::stopMark(const Train& t, int station) const {
  const auto& st = stations_[static_cast<size_t>(station)];
  return st.s + t.dir * t.cars * t.car_len * 0.5;  // train centred on the platform
}

void Trains::chooseNextStop(Train& t) const {
  const auto& L = lines_[static_cast<size_t>(t.line)];
  double best = 1e30;
  int pick = -1;
  for (int i = 0; i < static_cast<int>(stations_.size()); ++i) {
    if (stations_[static_cast<size_t>(i)].line != t.line) continue;
    double d = (stopMark(t, i) - t.s) * t.dir;
    if (L.closed) {
      d = std::fmod(d, L.length);
      if (d < 0) d += L.length;
    }
    if (d > 2.0 && d < best) {
      best = d;
      pick = i;
    }
  }
  t.next_stop = pick;
}

void Trains::markObstacles(const std::vector<rj::geo::Vec3d>& pts) {
  for (auto& c : crossings_) {
    c.obstacle = false;
    const double th = c.road_hd * 3.14159265358979 / 180.0, ux = std::sin(th), uy = std::cos(th);
    for (const auto& p : pts) {
      const double dx = p.x - c.pos.x, dy = p.y - c.pos.y;
      const double along = dx * ux + dy * uy, across = -dx * uy + dy * ux;  // along the road / across it
      if (std::fabs(along) < 3.6 && std::fabs(across) < c.half + 0.8 && std::fabs(p.z - c.pos.z) < 2.5) c.obstacle = true;
    }
  }
}

void Trains::updateCrossings(double dt) {
  // The warning starts when a train is due within about 35 s (or is within 150 m), and stops once
  // its last car has cleared the crossing; the arms come down 5 s after the lamps start and go up
  // 2 s after they stop (typical Japanese timings; game values).
  for (auto& c : crossings_) {
    const auto& L = lines_[static_cast<size_t>(c.line)];
    bool warn = false;
    for (const auto& t : trains_) {
      if (t.line != c.line) continue;
      double ahead = (c.s - t.s) * t.dir;  // from the front to the crossing, along the way it runs
      if (L.closed) ahead = std::remainder(ahead, L.length);
      const double len = t.cars * t.car_len;
      if (ahead < -len - 8.0) continue;          // passed
      if (ahead <= 0.0) warn = true;              // on the crossing
      else if (ahead < 150.0 || ahead / std::max(t.v, 4.0) < 35.0) warn = warn || t.v > 0.2 || ahead < 60.0;
    }
    static const bool force = std::getenv("RJ_XING_TEST") != nullptr;  // test aid: every crossing warning
    warn = warn || force;
    if (warn != c.warning) {
      c.warning = warn;
      c.since = 0;
    }
    c.since += static_cast<float>(dt);
    const float target = c.warning ? (c.since > 5.0f ? 1.0f : 0.0f) : (c.since > 2.0f ? 0.0f : c.arm > 0.0f ? 1.0f : 0.0f);
    const float rate = static_cast<float>(dt) / 6.0f;  // an arm takes about 6 s to come down / go up
    c.arm = target > c.arm ? std::min(target, c.arm + rate) : std::max(target, c.arm - rate);
  }
}

void Trains::update(double dt) {
  if (!placed_) return;
  updateCrossings(dt);
  for (auto& t : trains_) {
    const auto& L = lines_[static_cast<size_t>(t.line)];
    if (t.hold > 0) {
      t.hold -= dt;
      if (t.hold <= 0) t.offmap = false;
      continue;
    }
    if (t.at_station >= 0) {
      t.dwell -= dt;
      if (t.dwell <= 0) {
        t.at_station = -1;
        chooseNextStop(t);
      }
      continue;
    }
    const double amax = L.kind == LineKind::Shinkansen ? 0.7 : 0.9, brake = L.kind == LineKind::Shinkansen ? 0.8 : 1.0;
    if (t.manual) {
      // player's notches: power up to 0.9 m/s^2, service brake up to 1.0 m/s^2, emergency 1.4
      double a = t.notch > 0 ? amax * t.notch / 5.0 : t.notch <= -8 ? -1.4 : t.notch < 0 ? -brake * (-t.notch) / 7.0 : -0.03;
      // ATS: train ahead on the same track, overspeed
      bool ats = false;
      for (const auto& o : trains_) {
        if (&o == &t || o.line != t.line || o.dir != t.dir) continue;
        double gap = (o.s - t.dir * o.cars * o.car_len - t.s) * t.dir;
        if (L.closed) {
          gap = std::fmod(gap, L.length);
          if (gap < 0) gap += L.length;
        }
        if (gap > 0 && gap < 60.0 + t.v * t.v / (2.0 * brake) && t.v > 0.5) ats = true;
      }
      if (t.v > speedCap(t) + 1.0) ats = true;  // overspeed against the curve limit ahead (ATS-P-like)
      for (const auto& c : crossings_) {  // a crossing's obstacle stop signal ahead
        if (!c.obstacle || c.line != t.line) continue;
        const double ahead = (c.s - t.s) * t.dir;
        if (ahead > 0.0 && ahead < 60.0 + t.v * t.v / (2.0 * brake) && t.v > 0.5) ats = true;
      }
      if (ats) a = std::min(a, -1.2);
      ats_[static_cast<size_t>(t.id) % 64] = ats;
      t.v = std::max(0.0, t.v + a * dt);
      t.s += t.dir * t.v * dt;
      if (L.closed) {
        t.s = std::fmod(t.s, L.length);
        if (t.s < 0) t.s += L.length;
      }
      continue;
    }
    double target;
    if (t.next_stop >= 0) {
      target = stopMark(t, t.next_stop);
    } else {
      target = t.dir > 0 ? L.length - 3.0 : 3.0 + t.cars * t.car_len;  // turnaround at the end of the line
    }
    double dist = (target - t.s) * t.dir;
    if (L.closed) {
      dist = std::fmod(dist, L.length);
      if (dist < 0) dist += L.length;
    }
    // keep a safe distance behind the train ahead on the same track
    for (const auto& o : trains_) {
      if (&o == &t || o.line != t.line || o.dir != t.dir) continue;
      double gap = (o.s - t.dir * o.cars * o.car_len - t.s) * t.dir;
      if (L.closed) {
        gap = std::fmod(gap, L.length);
        if (gap < 0) gap += L.length;
      }
      if (gap > 0 && gap - 60.0 < dist) dist = std::max(0.0, gap - 60.0);
    }
    // a crossing's obstacle stop signal ahead (seen from about 600 m): stop short of the crossing,
    // braking up to the emergency rate (a train too close to stop in time runs over the crossing)
    bool emergency = false;
    for (const auto& c : crossings_) {
      if (!c.obstacle || c.line != t.line) continue;
      double ahead = (c.s - t.s) * t.dir;
      if (L.closed) ahead = std::remainder(ahead, L.length);
      if (ahead > 0.0 && ahead < 600.0 && ahead - 25.0 < dist) {
        dist = std::max(0.0, ahead - 25.0);
        emergency = true;
      }
    }
    if (dist <= 0.4 && t.v < 1.0) {
      if (emergency) {  // waiting at the stop signal until the crossing is clear
        t.v = 0;
        continue;
      }
      t.v = 0;
      if (t.next_stop >= 0 && std::fabs(distToStop(t)) < 3.0) {  // (distance wraps round the loop line)
        t.at_station = t.next_stop;
        const bool terminal = L.kind != LineKind::Loop;
        t.dwell = t.dwell0 = terminal ? 45.0 : 25.0;
      } else if (t.next_stop < 0) {
        // end of the line: reverse (the front becomes the other end) after a turnaround wait
        t.s -= t.dir * t.cars * t.car_len;
        t.dir = -t.dir;
        t.offmap = false;
        t.hold = 25.0;
        chooseNextStop(t);
      }
      continue;
    }
    // follow the braking curve to the stop mark (or to the safe distance behind the train ahead):
    // the speed never exceeds what service braking can take off in the distance left, so the
    // train arrives on the mark instead of sliding past it
    const double vcap = std::min(std::sqrt(2.0 * brake * std::max(0.0, dist - 0.1)), speedCap(t));
    if (t.v > vcap) t.v = std::max(vcap, t.v - (emergency ? 1.4 : 1.3 * brake) * dt);
    else t.v = std::min({t.vmax, t.v + amax * dt, std::max(vcap, 0.3)});
    const double adv = std::min(t.v * dt, std::max(0.0, dist));
    t.s += t.dir * adv;
    if (L.closed) {
      t.s = std::fmod(t.s, L.length);
      if (t.s < 0) t.s += L.length;
    }
  }
}

void Trains::setManual(int id, bool on) {
  for (auto& t : trains_)
    if (t.id == id) {
      t.manual = on;
      t.notch = 0;
      if (!on && t.at_station < 0) chooseNextStop(t);
    }
}

void Trains::setNotch(int id, int notch) {
  for (auto& t : trains_)
    if (t.id == id) t.notch = std::clamp(notch, -8, 5);
}

double Trains::distToStop(const Train& t) const {
  if (t.next_stop < 0) return 1e9;
  const auto& L = lines_[static_cast<size_t>(t.line)];
  double d = (stopMark(t, t.next_stop) - t.s) * t.dir;
  if (L.closed) {
    d = std::fmod(d, L.length);
    if (d < -L.length * 0.5) d += L.length;
    if (d > L.length * 0.5) d -= L.length;
  }
  return d;
}

bool Trains::openDoors(int id, double& stop_error) {
  for (auto& t : trains_)
    if (t.id == id && t.manual && t.v < 0.1 && t.next_stop >= 0 && t.at_station < 0) {
      const double d = distToStop(t);
      if (std::fabs(d) > 8.0) return false;
      stop_error = -d;  // + overran, - short of the mark
      t.at_station = t.next_stop;
      t.dwell = t.dwell0 = 20.0;
      return true;
    }
  return false;
}

const Train* Trains::train(int id) const {
  for (const auto& t : trains_)
    if (t.id == id) return &t;
  return nullptr;
}

void Trains::carPose(const Train& t, int k, rj::geo::Vec3d& pos, float& yaw, float& pitch) const {
  const auto& L = lines_[static_cast<size_t>(t.line)];
  const double s = t.s - t.dir * (k * t.car_len + t.car_len * 0.5);
  double heading, grade;
  pointAt(L, s, pos, heading, grade);
  yaw = static_cast<float>(t.dir > 0 ? heading : heading + kPi);
  pitch = static_cast<float>(std::atan(grade * t.dir));
  // Japanese railways keep left: each direction runs on its own track of the double line.
  const double off = trackOffset(L.kind);
  pos.x += -std::cos(yaw) * off;
  pos.y += std::sin(yaw) * off;
}

int Trains::stationNear(const rj::geo::Vec3d& p, double r) const {
  int best = -1;
  double bd = r;
  for (int i = 0; i < static_cast<int>(stations_.size()); ++i) {
    const double d = std::hypot(stations_[static_cast<size_t>(i)].pos.x - p.x, stations_[static_cast<size_t>(i)].pos.y - p.y);
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

int Trains::trainStoppedAt(int station) const {
  for (const auto& t : trains_)
    if (t.at_station == station && t.dwell > 3.0) return t.id;
  return -1;
}

std::string Trains::destination(const Train& t) const {
  const auto& L = lines_[static_cast<size_t>(t.line)];
  if (L.closed) return t.dir > 0 ? "loop+" : "loop-";
  // the last station in the direction of travel
  int pick = -1;
  double far = -1e30;
  for (int i = 0; i < static_cast<int>(stations_.size()); ++i) {
    if (stations_[static_cast<size_t>(i)].line != t.line) continue;
    const double d = stations_[static_cast<size_t>(i)].s * t.dir;
    if (d > far) {
      far = d;
      pick = i;
    }
  }
  return pick >= 0 ? stations_[static_cast<size_t>(pick)].name : "";
}

double Trains::speedCap(const Train& t) const {
  const auto& L = lines_[static_cast<size_t>(t.line)];
  if (L.env_fwd.empty()) return t.vmax;
  double s = t.s;
  if (L.closed) {
    s = std::fmod(s, L.length);
    if (s < 0) s += L.length;
  }
  const size_t k = std::min(L.cum.size() - 1, static_cast<size_t>(std::lower_bound(L.cum.begin(), L.cum.end(), s) - L.cum.begin()));
  // the front must respect what lies ahead; the rear the limit it is still passing
  double cap = t.dir > 0 ? L.env_fwd[k] : L.env_bwd[k];
  const double rear = s - t.dir * t.cars * t.car_len;
  const double rs = L.closed ? std::fmod(std::fmod(rear, L.length) + L.length, L.length) : std::clamp(rear, 0.0, L.length);
  const size_t a = std::min(L.cum.size() - 1, static_cast<size_t>(std::lower_bound(L.cum.begin(), L.cum.end(), std::min(s, rs)) - L.cum.begin()));
  const size_t b = std::min(L.cum.size() - 1, static_cast<size_t>(std::lower_bound(L.cum.begin(), L.cum.end(), std::max(s, rs)) - L.cum.begin()));
  if (b >= a && b - a < 200)  // (not across the loop's seam)
    for (size_t i = a; i <= b; ++i) cap = std::min(cap, L.vlim[i]);
  return std::min(cap, t.vmax);
}

}  // namespace rjc
