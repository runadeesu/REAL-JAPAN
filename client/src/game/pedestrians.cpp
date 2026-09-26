#include "game/pedestrians.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "rj/sim/rng.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

constexpr double kWalkSpeed = 1.4;  // m/s, real time

double segDist(const rj::nav::Vec2& p, const rj::nav::Vec2& a, const rj::nav::Vec2& b) {
  const double vx = b.x - a.x, vy = b.y - a.y;
  const double l2 = vx * vx + vy * vy;
  double t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

void styleFor(Walker& w, const rj::sim::Npc& n) {
  static const Color shirts[] = {{235, 235, 232, 255}, {40, 52, 84, 255},  {30, 30, 32, 255},   {128, 130, 134, 255},
                                 {196, 180, 150, 255}, {150, 180, 210, 255}, {96, 104, 72, 255}, {118, 40, 44, 255}};
  static const Color pants[] = {{28, 28, 30, 255}, {38, 46, 72, 255}, {96, 96, 100, 255}, {170, 150, 120, 255}, {60, 80, 120, 255}};
  static const Color skins[] = {{236, 204, 176, 255}, {222, 186, 150, 255}, {204, 166, 132, 255}};
  static const Color hairs[] = {{22, 18, 16, 255}, {30, 24, 20, 255}, {58, 40, 28, 255}, {92, 66, 44, 255}, {150, 150, 150, 255}};
  rj::sim::Rng r(n.seed ^ 0x5eedULL);
  w.shirt = shirts[r.next() % 8];
  w.pants = pants[r.next() % 5];
  w.skin = skins[r.next() % 3];
  w.hair = n.age > 62 ? hairs[4] : hairs[r.next() % 4];
  w.variant = static_cast<int>(r.next() % 3);
  w.height_scale = n.age < 13 ? 0.78f : n.age < 16 ? 0.92f : static_cast<float>(r.uniform(0.94, 1.06));
  w.phase = static_cast<float>(r.uniform(0.0, 6.28));
}

}  // namespace

Pedestrians::~Pedestrians() {
  if (job_.valid()) job_.wait();
}

void Pedestrians::clear() {
  if (job_.valid()) job_.wait();
  job_ = {};
  walkers_.clear();
  pending_.clear();
  failed_.clear();
  queue_.clear();
  last_minute_ = -1;
}

void Pedestrians::buildNav(const World& world) {
  clear();
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const auto& [code, c] : world.cells())
    for (size_t k = 0; k < c->tx.size(); ++k) {
      x0 = std::min(x0, c->tx[k]);
      y0 = std::min(y0, c->ty[k]);
      x1 = std::max(x1, c->tx[k]);
      y1 = std::max(y1, c->ty[k]);
    }
  if (x1 <= x0) return;
  const double cell = 2.0;
  nav_ = rj::nav::GridNav(static_cast<int>((x1 - x0) / cell) + 1, static_cast<int>((y1 - y0) / cell) + 1, cell, x0, y0);
  for (const auto& [code, c] : world.cells())
    for (const auto& fp : c->fp) {
      std::vector<rj::nav::Vec2> poly;
      poly.reserve(fp.size());
      for (const auto& p : fp) poly.push_back({p.x, p.y});
      nav_.blockPolygon(poly);
    }
  // Stairwell openings of verified underground spaces: nobody walks across the hole.
  for (const auto& im : world.meta().interiors)
    for (const auto& ring : im.openings) {
      std::vector<rj::nav::Vec2> poly;
      for (const auto& g : ring) {
        const auto p = world.toLocal(g);
        poly.push_back({p.x, p.y});
      }
      nav_.blockPolygon(poly);
    }
  nav_.computeComponents();
  if (std::getenv("RJ_DEBUG")) {  // dump the walkability grid for inspection
    Image img = GenImageColor(nav_.width(), nav_.height(), WHITE);
    size_t nblocked = 0;
    for (int y = 0; y < nav_.height(); ++y)
      for (int x = 0; x < nav_.width(); ++x)
        if (nav_.blocked(x, y)) {
          ImageDrawPixel(&img, x, nav_.height() - 1 - y, BLACK);
          ++nblocked;
        }
    ExportImage(img, "nav_debug.png");
    UnloadImage(img);
    TraceLog(LOG_DEBUG, "RJ: nav grid %dx%d, blocked %zu", nav_.width(), nav_.height(), nblocked);
  }
}

void Pedestrians::startJobs() {
  if (job_.valid() || queue_.empty() || !nav_.valid()) return;
  std::vector<Job> batch;
  while (!queue_.empty() && batch.size() < 16) {
    batch.push_back(queue_.back());
    queue_.pop_back();
  }
  const rj::nav::GridNav* nav = &nav_;
  job_ = std::async(std::launch::async, [nav, batch]() {
    std::vector<Result> out;
    for (const auto& j : batch) out.push_back({j.npc, j.trip, nav->findPath(j.from, j.to, 250000)});
    return out;
  });
}

void Pedestrians::update(TownSim& town, const World& world, const rj::sim::CivilDateTime& now,
                         const rj::geo::Vec3d& player, float real_dt) {
  if (!nav_.valid()) return;
  const rj::nav::Vec2 me{player.x, player.y};
  const int minute = now.minuteOfDay();
  auto local = [&](const rj::sim::PlaceRef& p) {
    const auto v = world.toLocal({p.pos.lat_deg, p.pos.lon_deg, 0.0});
    return rj::nav::Vec2{v.x, v.y};
  };

  // Re-evaluate who is walking once per game minute.
  if (minute != last_minute_ || !(now.date == last_date_)) {
    last_minute_ = minute;
    last_date_ = now.date;
    trips_ = town.walkingTrips(now.date, minute);
    struct Cand {
      WalkTrip t;
      rj::nav::Vec2 a, b;
      double d;
    };
    std::vector<Cand> cands;
    for (const auto& t : trips_) {
      const auto a = local(t.from), b = local(t.to);
      const double d = segDist(me, a, b);
      if (d < kVisibleRadius) cands.push_back({t, a, b, d});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
    TraceLog(LOG_DEBUG, "RJ: peds minute=%d trips=%zu near=%zu walkers=%zu pending=%zu failed=%zu", minute, trips_.size(),
             cands.size(), walkers_.size(), pending_.size(), failed_.size());
    if (cands.size() > kMaxWalkers) cands.resize(kMaxWalkers);
    for (const auto& c : cands) {
      const size_t id = c.t.npc;
      if (walkers_.count(id) || (pending_.count(id) && pending_[id] == c.t.start_min)) continue;
      if (failed_.count(id) && failed_[id] == c.t.start_min) continue;
      pending_[id] = c.t.start_min;
      queue_.push_back({id, c.t, c.a, c.b});
    }
  }

  // Collect finished routes.
  if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto results = job_.get();
    size_t ok = 0;
    for (const auto& r : results) ok += (r.path && r.path->size() >= 2) ? 1 : 0;
    TraceLog(LOG_DEBUG, "RJ: peds batch %zu routes (%zu ok), queue=%zu walkers=%zu", results.size(), ok, queue_.size(),
             walkers_.size());
    for (auto& r : results) {
      pending_.erase(r.npc);
      if (!r.path || r.path->size() < 2) {
        failed_[r.npc] = r.trip.start_min;
        continue;
      }
      Walker w;
      w.npc = r.npc;
      w.trip = r.trip;
      w.path = std::move(*r.path);
      w.length = rj::nav::GridNav::pathLength(w.path);
      const double span = std::max(1, r.trip.end_min - r.trip.start_min);
      const double f = std::clamp((minute + now.second / 60.0 - r.trip.start_min) / span, 0.0, 1.0);
      w.dist = f * w.length;
      styleFor(w, town.npc(r.npc));
      walkers_[r.npc] = std::move(w);
    }
    job_ = {};
  }
  startJobs();

  // Advance at a real walking pace; drop walkers that arrived or are far away.
  for (auto it = walkers_.begin(); it != walkers_.end();) {
    Walker& w = it->second;
    w.dist += kWalkSpeed * real_dt;
    w.phase += real_dt * 7.5f;
    w.pos = rj::nav::GridNav::pointAt(w.path, w.dist, &w.dir);
    if (auto h = world.surfaceHeight(w.pos.x, w.pos.y)) w.z = static_cast<float>(*h);
    const bool far = std::hypot(w.pos.x - me.x, w.pos.y - me.y) > kVisibleRadius * 1.3;
    if (w.dist >= w.length || far) it = walkers_.erase(it);
    else ++it;
  }
}

const Walker* Pedestrians::pick(const rj::geo::Vec3d& eye, const rj::geo::Vec3d& dir, double max_dist) const {
  const Walker* best = nullptr;
  double best_t = max_dist;
  for (const auto& [id, w] : walkers_) {
    // Closest approach of the view ray to the walker's vertical axis.
    const double dx = w.pos.x - eye.x, dy = w.pos.y - eye.y;
    const double h2 = dir.x * dir.x + dir.y * dir.y;
    if (h2 < 1e-9) continue;
    const double t = (dx * dir.x + dy * dir.y) / h2;
    if (t <= 0 || t > best_t) continue;
    const double px = eye.x + dir.x * t - w.pos.x, py = eye.y + dir.y * t - w.pos.y;
    const double pz = eye.z + dir.z * t - w.z;
    if (std::hypot(px, py) < 0.45 && pz > 0 && pz < 1.8 * w.height_scale) {
      best = &w;
      best_t = t;
    }
  }
  return best;
}

}  // namespace rjc
