#include "game/traffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "game/traffic_signals.hpp"
#include "platform/paths.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

constexpr double kPi = 3.14159265358979323846;
double wrapAngle(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

}  // namespace

float Traffic::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ & 0xffffff) / 16777215.0f;
}

float Traffic::lengthOf(VehicleType t) {
  switch (t) {
    case VehicleType::Kei: return 3.4f;
    case VehicleType::Taxi: return 4.4f;
    case VehicleType::Minivan: return 4.9f;
    case VehicleType::Van: return 4.7f;
    case VehicleType::Truck: return 6.2f;
    case VehicleType::Bus: return 10.5f;
    default: return 4.7f;
  }
}

bool Traffic::load(const std::filesystem::path& file, std::string& err) {
  auto d = readFile(file);
  if (!d || d->size() < 12 || std::memcmp(d->data(), "RJROAD01", 8) != 0) {
    err = "no road graph";
    return false;
  }
  size_t p = 8;
  auto rd = [&](void* dst, size_t n) {
    if (p + n > d->size()) return false;
    std::memcpy(dst, d->data() + p, n);
    p += n;
    return true;
  };
  uint32_t nn = 0;
  if (!rd(&nn, 4)) return false;
  nodes_.resize(nn);
  for (auto& n : nodes_) {
    double ll[2];
    if (!rd(ll, 16)) return false;
    n.geo = {ll[0], ll[1], 0.0};
  }
  uint32_t ne = 0;
  if (!rd(&ne, 4)) return false;
  edges_.resize(ne);
  for (uint32_t i = 0; i < ne; ++i) {
    Edge& e = edges_[i];
    uint32_t a, b, np;
    float w;
    if (!rd(&a, 4) || !rd(&b, 4) || !rd(&w, 4) || !rd(&np, 4) || a >= nn || b >= nn) {
      err = "bad road edge";
      return false;
    }
    e.a = static_cast<int>(a);
    e.b = static_cast<int>(b);
    e.width = w;
    e.geo.resize(np);
    for (auto& g : e.geo) {
      double ll[2];
      if (!rd(ll, 16)) return false;
      g = {ll[0], ll[1], 0.0};
    }
    // Lanes from the measured carriageway width (Japanese lane width ~3.0-3.5 m).
    e.lanes = std::clamp(static_cast<int>(w / 6.3f), 1, 3);
    e.lane_w = std::min(3.3, static_cast<double>(w) / (2.0 * e.lanes));
    e.v0 = w > 12.0f ? 50.0 / 3.6 : (w > 7.0f ? 40.0 / 3.6 : 25.0 / 3.6);
    nodes_[a].edges.push_back(static_cast<int>(i));
    if (b != a) nodes_[b].edges.push_back(static_cast<int>(i));
  }
  return true;
}

void Traffic::place(const World& world) {
  for (auto& n : nodes_) n.pos = world.toLocal(n.geo);
  for (auto& e : edges_) {
    e.pts.clear();
    e.cum.clear();
    double acc = 0;
    for (size_t k = 0; k < e.geo.size(); ++k) {
      rj::geo::Vec3d q = world.toLocal(e.geo[k]);
      if (auto h = world.roadHeight(q.x, q.y)) q.z = *h;
      if (!e.pts.empty()) acc += std::hypot(q.x - e.pts.back().x, q.y - e.pts.back().y);
      e.pts.push_back(q);
      e.cum.push_back(acc);
    }
    e.length = acc;
  }
  veh_.clear();  // positions are recomputed from (edge, s); simply respawn around the player
  cand_.clear();
  warm_frames_ = 0;
}

double Traffic::networkKm() const {
  double s = 0;
  for (const auto& e : edges_) s += e.length;
  return s / 1000.0;
}

void Traffic::samplePose(const Edge& e, int dir, int lane, double s, rj::geo::Vec3d& p, double& heading) const {
  const double L = e.length;
  double u = dir == 0 ? s : L - s;  // position along the stored polyline
  u = std::clamp(u, 0.0, L);
  size_t k = static_cast<size_t>(std::upper_bound(e.cum.begin(), e.cum.end(), u) - e.cum.begin());
  k = std::clamp<size_t>(k, 1, e.pts.size() - 1);
  const auto& A = e.pts[k - 1];
  const auto& B = e.pts[k];
  const double seg = std::max(1e-6, e.cum[k] - e.cum[k - 1]);
  const double t = (u - e.cum[k - 1]) / seg;
  double tx = B.x - A.x, ty = B.y - A.y;
  const double l = std::max(1e-9, std::hypot(tx, ty));
  tx /= l;
  ty /= l;
  if (dir == 1) {
    tx = -tx;
    ty = -ty;
  }
  // Left-hand traffic: lanes lie to the left of the travel direction.
  const double lx = -ty, ly = tx;
  const double off = e.lane_w * (e.lanes - lane - 0.5) + (e.lanes == 1 && e.width < 5.5 ? -e.lane_w * 0.25 : 0.0);
  p = {A.x + (B.x - A.x) * t + lx * off, A.y + (B.y - A.y) * t + ly * off, A.z + (B.z - A.z) * t};
  heading = std::atan2(tx, ty);
}

double Traffic::headingAtEnd(const Edge& e, int dir) const {
  const size_t n = e.pts.size();
  const auto& A = dir == 0 ? e.pts[n - 2] : e.pts[1];
  const auto& B = dir == 0 ? e.pts[n - 1] : e.pts[0];
  return std::atan2(B.x - A.x, B.y - A.y);
}

double Traffic::headingAtStart(const Edge& e, int dir) const {
  const size_t n = e.pts.size();
  const auto& A = dir == 0 ? e.pts[0] : e.pts[n - 1];
  const auto& B = dir == 0 ? e.pts[1] : e.pts[n - 2];
  return std::atan2(B.x - A.x, B.y - A.y);
}

void Traffic::chooseNext(Vehicle& v) {
  const Edge& e = edges_[static_cast<size_t>(v.edge)];
  const int node = v.dir == 0 ? e.b : e.a;
  const double hin = headingAtEnd(e, v.dir);
  std::vector<std::pair<int, int>> opts;
  std::vector<float> w;
  for (int ei : nodes_[static_cast<size_t>(node)].edges) {
    const Edge& o = edges_[static_cast<size_t>(ei)];
    for (int d = 0; d < 2; ++d) {
      if ((d == 0 ? o.a : o.b) != node) continue;
      if (ei == v.edge && d != v.dir) continue;  // no U-turns
      if (o.length < 1.0) continue;
      const double hout = std::atan2((d == 0 ? o.pts[1] : o.pts[o.pts.size() - 2]).x - (d == 0 ? o.pts[0] : o.pts.back()).x,
                                     (d == 0 ? o.pts[1] : o.pts[o.pts.size() - 2]).y - (d == 0 ? o.pts[0] : o.pts.back()).y);
      const double turn = std::fabs(wrapAngle(hout - hin));
      if (turn > 2.6) continue;
      // prefer going straight and staying on wide roads
      float weight = static_cast<float>(1.2 - turn / 3.0) * (0.4f + std::min(o.width, 20.0f) / 12.0f);
      if (v.type == VehicleType::Bus && o.width < 9.0f) weight *= 0.05f;
      opts.push_back({ei, d});
      w.push_back(std::max(0.02f, weight));
    }
  }
  if (opts.empty()) {
    v.next_edge = v.edge;  // dead end: turn around
    v.next_dir = 1 - v.dir;
    return;
  }
  float tot = 0;
  for (float x : w) tot += x;
  float r = rnd() * tot;
  for (size_t i = 0; i < opts.size(); ++i) {
    r -= w[i];
    if (r <= 0 || i + 1 == opts.size()) {
      v.next_edge = opts[i].first;
      v.next_dir = opts[i].second;
      break;
    }
  }
}

void Traffic::clearRoadMarkingState() {
  for (auto& n : nodes_) n.est_group = -1;
  for (auto& e : edges_) e.stop[0] = e.stop[1] = -1.0f;
}

void Traffic::assignSignals(const TrafficSignals& signals) {
  for (auto& n : nodes_) {
    n.signal_group = n.est_group;
    if (n.est_group >= 0) continue;
    double best = 32.0;
    for (const auto& h : signals.heads()) {
      if (h.kind != 0 || h.estimated) continue;
      const double d = std::hypot(h.pos.x - n.pos.x, h.pos.y - n.pos.y);
      if (d < best) {
        best = d;
        n.signal_group = h.group;
      }
    }
  }
}

bool Traffic::spawnOne(const rj::geo::Vec3d& player, double rmin, double rmax) {
  // Candidate edges near the player, weighted by carriageway width (major roads carry more traffic).
  if (std::hypot(player.x - cand_at_.x, player.y - cand_at_.y) > 40.0 || cand_.empty()) {
    cand_.clear();
    cand_w_.clear();
    cand_at_ = player;
    for (size_t i = 0; i < edges_.size(); ++i) {
      const Edge& e = edges_[i];
      if (e.length < 12.0 || e.width < 4.0f) continue;
      double dmin = 1e30;
      for (const auto& q : e.pts) dmin = std::min(dmin, std::hypot(q.x - player.x, q.y - player.y));
      if (dmin > rmax) continue;
      cand_.push_back(static_cast<int>(i));
      cand_w_.push_back(static_cast<float>(e.length) * std::min(e.width, 22.0f) * std::min(e.width, 22.0f) / 64.0f);
    }
  }
  if (cand_.empty()) return false;
  float tot = 0;
  for (float w : cand_w_) tot += w;
  for (int attempt = 0; attempt < 16; ++attempt) {
    float pr = rnd() * tot;
    size_t pick = 0;
    for (; pick + 1 < cand_.size(); ++pick) {
      pr -= cand_w_[pick];
      if (pr <= 0) break;
    }
    const int ei = cand_[pick];
    Edge& e = edges_[static_cast<size_t>(ei)];
    if (e.length < 12.0 || e.width < 4.0f) continue;
    const double s = 3.0 + rnd() * (e.length - 6.0);
    const int dir = rnd() < 0.5f ? 0 : 1;
    const int lane = static_cast<int>(rnd() * (e.lanes - 0.001));
    rj::geo::Vec3d p;
    double hd;
    samplePose(e, dir, lane, s, p, hd);
    const double d = std::hypot(p.x - player.x, p.y - player.y);
    if (d < rmin || d > rmax) continue;
    bool clear = true;
    for (const auto& o : veh_)
      if (o.edge == ei && o.dir == dir && o.lane == lane && std::fabs(o.s - s) < 14.0) clear = false;
    if (!clear) continue;
    Vehicle v;
    v.id = next_id_++;
    const float r = rnd();
    v.type = r < 0.30f ? VehicleType::Sedan : r < 0.50f ? VehicleType::Taxi : r < 0.64f ? VehicleType::Kei
           : r < 0.76f ? VehicleType::Minivan : r < 0.86f ? VehicleType::Van : r < 0.94f ? VehicleType::Truck : VehicleType::Bus;
    if (v.type == VehicleType::Bus && e.width < 9.0f) v.type = VehicleType::Sedan;
    // Paint: Japanese colour mix (pearl white, black, silver, grey, blue, red ...).
    static const float paints[][3] = {{0.92f, 0.92f, 0.90f}, {0.93f, 0.93f, 0.93f}, {0.03f, 0.03f, 0.035f}, {0.03f, 0.03f, 0.03f},
                                      {0.55f, 0.56f, 0.58f}, {0.30f, 0.31f, 0.33f}, {0.08f, 0.16f, 0.38f}, {0.50f, 0.05f, 0.06f},
                                      {0.62f, 0.56f, 0.45f}, {0.20f, 0.30f, 0.22f}};
    const float* c = paints[static_cast<int>(rnd() * 9.99f)];
    if (v.type == VehicleType::Taxi) {
      static const float taxi[][3] = {{0.05f, 0.07f, 0.14f}, {0.05f, 0.07f, 0.14f}, {0.60f, 0.50f, 0.05f}, {0.05f, 0.25f, 0.10f}};
      c = taxi[static_cast<int>(rnd() * 3.99f)];  // JPN Taxi indigo, yellow, green
    }
    if (v.type == VehicleType::Van || v.type == VehicleType::Truck) c = paints[0];
    if (v.type == VehicleType::Bus) {
      static const float bus[3] = {0.86f, 0.88f, 0.84f};
      c = bus;
    }
    std::memcpy(v.color, c, sizeof v.color);
    v.edge = ei;
    v.dir = dir;
    v.lane = lane;
    v.s = s;
    v.v = e.v0 * (0.5 + 0.4 * rnd());
    v.pos = p;
    v.yaw = static_cast<float>(hd);
    chooseNext(v);
    veh_.push_back(v);
    return true;
  }
  return false;
}

void Traffic::update(double dt, const World& world, const TrafficSignals& signals, const rj::geo::Vec3d& player, int hour) {
  if (edges_.empty()) return;
  dt = std::clamp(dt, 0.0, 0.1);
  signal_check_t_ -= dt;
  if (signal_check_t_ <= 0) {
    assignSignals(signals);
    signal_check_t_ = 5.0;
  }
  // Traffic volume by hour (vehicles alive around the player).
  static const int kByHour[24] = {40, 28, 20, 18, 22, 40, 80, 130, 150, 130, 115, 115, 120, 115, 115, 120, 130, 150, 150, 125, 100, 85, 70, 55};
  const int target = kByHour[std::clamp(hour, 0, 23)];
  for (auto it = veh_.begin(); it != veh_.end();)
    if (std::hypot(it->pos.x - player.x, it->pos.y - player.y) > 420.0) it = veh_.erase(it);
    else ++it;
  // Fill the whole radius at start, then keep topping up out of sight (> 150 m).
  const bool initial = warm_frames_ < 3;
  int budget = initial ? target : 3;
  while (static_cast<int>(veh_.size()) < target && budget-- > 0)
    if (!spawnOne(player, initial ? 12.0 : 150.0, 380.0)) break;
  ++warm_frames_;

  // Lane occupancy for leader search.
  std::map<std::tuple<int, int, int>, std::vector<size_t>> lanes;
  for (size_t i = 0; i < veh_.size(); ++i) lanes[{veh_[i].edge, veh_[i].dir, veh_[i].lane}].push_back(i);
  const double T = 1.4, s0 = 2.2, amax = 1.3, bcomf = 2.2;
  std::vector<double> accs(veh_.size());
  for (size_t i = 0; i < veh_.size(); ++i) {
    Vehicle& v = veh_[i];
    const Edge& e = edges_[static_cast<size_t>(v.edge)];
    const double len = lengthOf(v.type);
    double gap = 1e9, dv = 0;
    for (size_t j : lanes[{v.edge, v.dir, v.lane}]) {
      if (j == i) continue;
      const Vehicle& o = veh_[j];
      const double d = o.s - v.s;
      if (d > 0 && d - lengthOf(o.type) * 0.5 - len * 0.5 < gap) {
        gap = d - lengthOf(o.type) * 0.5 - len * 0.5;
        dv = v.v - o.v;
      }
    }
    if (obstacle_on_) {  // the player's car ahead in this lane
      const double hx = std::sin(v.yaw), hy = std::cos(v.yaw);
      const double rx = obstacle_.x - v.pos.x, ry = obstacle_.y - v.pos.y;
      const double along = rx * hx + ry * hy, lat = std::fabs(rx * hy - ry * hx);
      if (along > 0 && along < 45.0 && lat < 2.3) {
        const double g = along - len * 0.5 - 2.4;
        if (g < gap) {
          gap = std::max(0.1, g);
          dv = v.v;
        }
      }
    }
    const double remain = e.length - v.s;
    // Look into the next edge for leaders near its start.
    if (remain < 30.0 && v.next_edge >= 0) {
      const Edge& ne = edges_[static_cast<size_t>(v.next_edge)];
      const int nl = std::min(v.lane, ne.lanes - 1);
      for (size_t j : lanes[{v.next_edge, v.next_dir, nl}]) {
        const Vehicle& o = veh_[j];
        const double d = remain + o.s;
        if (d - lengthOf(o.type) * 0.5 - len * 0.5 < gap) {
          gap = d - lengthOf(o.type) * 0.5 - len * 0.5;
          dv = v.v - o.v;
        }
      }
    }
    // Junction control: real signals, otherwise slow down and yield.
    const int node = v.dir == 0 ? e.b : e.a;
    const Node& N = nodes_[static_cast<size_t>(node)];
    const float stop_d = e.stop[v.dir == 0 ? 1 : 0];  // stop line (estimated markings) or a default
    // IDM keeps s0 (2.2 m) to the stop point: place it just past the line so the bumper stops at it.
    const double stop_at = std::max(0.0, e.length - (stop_d > 0 ? stop_d - 1.9 : std::min(9.0, e.length * 0.4)));
    if (N.signal_group >= 0) {
      const double h = headingAtEnd(e, v.dir);
      // phase of the head facing this approach (anti-parallel to the travel direction)
      int phase = 0;
      double bestd = 10;
      for (const auto& hd : signals.heads())
        if (hd.kind == 0 && hd.group == N.signal_group) {
          const double dd = std::fabs(wrapAngle(hd.facing - (h + kPi)));
          if (dd < bestd) {
            bestd = dd;
            phase = hd.phase;
          }
        }
      const VehLamp lamp = signals.vehicle(N.signal_group, phase);
      const double to_stop = stop_at - v.s;
      const bool must_stop = lamp == VehLamp::Red || (lamp == VehLamp::Yellow && to_stop > v.v * v.v / (2 * 3.5));
      if (must_stop && to_stop > -0.5 && to_stop < gap + len * 0.5) {
        gap = std::max(0.1, to_stop - len * 0.5);
        dv = v.v;
      }
    } else if (remain < 18.0) {
      // unsignalised: approach slowly; yield if another vehicle is in the junction
      bool busy = false;
      for (const auto& o : veh_)
        if (o.id != v.id && std::hypot(o.pos.x - N.pos.x, o.pos.y - N.pos.y) < 5.0 && o.edge != v.edge) busy = true;
      const double vmax = busy ? 0.0 : 5.0;
      if (v.v > vmax && remain > 3.0) {
        const double to_stop = remain - 3.0;
        if (to_stop < gap) {
          gap = std::max(0.1, to_stop);
          dv = v.v - vmax;
        }
      }
    }
    const double v0 = e.v0 * (v.type == VehicleType::Bus || v.type == VehicleType::Truck ? 0.85 : 1.0);
    const double sstar = s0 + std::max(0.0, v.v * T + v.v * dv / (2.0 * std::sqrt(amax * bcomf)));
    double a = amax * (1.0 - std::pow(v.v / std::max(0.1, v0), 4.0) - std::pow(sstar / std::max(0.2, gap), 2.0));
    accs[i] = std::clamp(a, -8.0, amax);
  }
  for (size_t i = 0; i < veh_.size(); ++i) {
    Vehicle& v = veh_[i];
    v.acc = accs[i];
    v.braking = v.acc < -0.8 || (v.v < 0.3 && v.acc < 0.05);
    v.v = std::max(0.0, v.v + v.acc * dt);
    v.s += v.v * dt;
    v.wheel_dist = static_cast<float>(std::fmod(v.wheel_dist + v.v * dt, 1000.0));
    while (true) {
      const Edge& e = edges_[static_cast<size_t>(v.edge)];
      if (v.s <= e.length) break;
      v.s -= e.length;
      v.edge = v.next_edge;
      v.dir = v.next_dir;
      v.lane = std::min(v.lane, edges_[static_cast<size_t>(v.edge)].lanes - 1);
      chooseNext(v);
    }
    const Edge& e = edges_[static_cast<size_t>(v.edge)];
    // indicate a turn at the next junction from about 30 m before it (the law asks for 30 m)
    int want = 0;
    if (v.next_edge >= 0 && e.length - v.s < 32.0) {
      const double turn = std::remainder(headingAtStart(edges_[static_cast<size_t>(v.next_edge)], v.next_dir) - headingAtEnd(e, v.dir), 2.0 * M_PI);
      want = turn > 0.6 ? 1 : (turn < -0.6 ? -1 : 0);
    }
    if (want != 0) {
      v.blink = want;
      v.blink_t = 1.2f;
    } else if ((v.blink_t -= static_cast<float>(dt)) <= 0.0f) {
      v.blink = 0;
    }
    rj::geo::Vec3d p;
    double hd;
    samplePose(e, v.dir, v.lane, v.s, p, hd);
    // smooth the heading through polyline corners / junctions
    const double dy = wrapAngle(hd - v.yaw);
    const double dyaw = dy * std::min(1.0, dt * 6.0);
    v.yaw = static_cast<float>(wrapAngle(v.yaw + dyaw));
    // front wheels follow the turn (kinematic steering angle from the yaw rate)
    if (dt > 1e-4) {
      const double want = std::clamp(std::atan(2.7 * dyaw / dt / std::max(v.v, 1.5)), -0.55, 0.55);
      v.steer = static_cast<float>(v.steer + (want - v.steer) * std::min(1.0, dt * 5.0));
    }
    if (auto h = world.roadHeight(p.x, p.y)) p.z = *h;
    v.pos = p;
  }
}

bool Traffic::take(int id, Vehicle& out) {
  for (auto it = veh_.begin(); it != veh_.end(); ++it)
    if (it->id == id) {
      out = *it;
      veh_.erase(it);
      return true;
    }
  return false;
}

bool Traffic::nearestLane(const rj::geo::Vec3d& p, double yaw_hint, rj::geo::Vec3d& out, double& heading) const {
  double best = 1e30, best_u = 0;
  int best_e = -1;
  for (size_t ei = 0; ei < edges_.size(); ++ei) {
    const Edge& e = edges_[ei];
    for (size_t k = 1; k < e.pts.size(); ++k) {
      const auto& A = e.pts[k - 1];
      const auto& B = e.pts[k];
      const double vx = B.x - A.x, vy = B.y - A.y, l2 = vx * vx + vy * vy;
      const double t = l2 > 0 ? std::clamp(((p.x - A.x) * vx + (p.y - A.y) * vy) / l2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(p.x - (A.x + t * vx), p.y - (A.y + t * vy));
      if (d < best) {
        best = d;
        best_e = static_cast<int>(ei);
        best_u = e.cum[k - 1] + t * std::sqrt(l2);
      }
    }
  }
  if (best_e < 0) return false;
  const Edge& e = edges_[static_cast<size_t>(best_e)];
  double h0;
  samplePose(e, 0, 0, best_u, out, h0);
  const int dir = std::cos(h0 - yaw_hint) >= 0 ? 0 : 1;
  samplePose(e, dir, 0, dir == 0 ? best_u : e.length - best_u, out, heading);
  return true;
}

}  // namespace rjc
