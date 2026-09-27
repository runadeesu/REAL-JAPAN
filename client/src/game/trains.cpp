#include "game/trains.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

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
    } else if (L.kind == LineKind::Branch) {
      mk(1, 220.0, 6, 20.0, 22.0);
      mk(-1, L.length - 20.0, 6, 20.0, 22.0);
    } else {
      mk(1, 205.0, 8, 25.0, 75.0);            // departing the terminal
      mk(-1, L.length - 10.0, 8, 25.0, 75.0);  // arriving from the mainland
    }
  }
  // Trains starting at a terminal wait there with the doors open (first departure).
  for (auto& t : trains_) {
    const auto& L = lines_[static_cast<size_t>(t.line)];
    if (L.closed || t.dir < 0) continue;
    int first = -1;
    for (int i = 0; i < static_cast<int>(stations_.size()); ++i)
      if (stations_[static_cast<size_t>(i)].line == t.line &&
          (first < 0 || stations_[static_cast<size_t>(i)].s < stations_[static_cast<size_t>(first)].s))
        first = i;
    if (first < 0) continue;
    t.s = stopMark(t, first);
    t.at_station = first;
    t.dwell = t.dwell0 = 60.0;
    t.dwell0 = 63.0;  // already open at the start
    t.next_stop = first;
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

void Trains::update(double dt) {
  if (!placed_) return;
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
      if (t.v > t.vmax + 1.0) ats = true;
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
    if (dist <= 0.4 && t.v < 1.0) {
      t.v = 0;
      if (t.next_stop >= 0 && std::fabs(distToStop(t)) < 3.0) {  // (distance wraps round the loop line)
        t.at_station = t.next_stop;
        const bool terminal = L.kind != LineKind::Loop;
        t.dwell = t.dwell0 = terminal ? 45.0 : 25.0;
      } else if (t.next_stop < 0) {
        // end of the line: reverse (the front becomes the other end); the Shinkansen's far end is
        // off the map (towards the mainland): it waits there before coming back
        t.s -= t.dir * t.cars * t.car_len;
        t.dir = -t.dir;
        t.offmap = L.kind == LineKind::Shinkansen && t.dir < 0;
        t.hold = t.offmap ? 150.0 : 25.0;
        chooseNextStop(t);
      }
      continue;
    }
    // follow the braking curve to the stop mark (or to the safe distance behind the train ahead):
    // the speed never exceeds what service braking can take off in the distance left, so the
    // train arrives on the mark instead of sliding past it
    const double vcap = std::sqrt(2.0 * brake * std::max(0.0, dist - 0.1));
    if (t.v > vcap) t.v = std::max(vcap, t.v - 1.3 * brake * dt);
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
  if (L.kind == LineKind::Shinkansen && t.dir > 0) return "mainland";
  return pick >= 0 ? stations_[static_cast<size_t>(pick)].name : "";
}

}  // namespace rjc
