#include "game/pedestrians.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "game/road_markings.hpp"
#include "game/traffic.hpp"
#include "game/traffic_signals.hpp"
#include "rj/sim/rng.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Visitors alive around the player at each hour where the surroundings are as busy as the
// Shibuya station area (activity 1). A game assumption, not measured people-flow data.
constexpr int kVisitorsByHour[24] = {90, 50, 25, 15, 12, 25, 90, 300, 420, 320, 300, 350,
                                     400, 390, 390, 420, 450, 490, 520, 500, 450, 380, 280, 160};
// Weighted floor area within 250 m that counts as activity 1 (calibrated on the station area).
constexpr double kActivityRef = 900000.0;
constexpr double kActivityRadius = 250.0;
constexpr double kSpawnRadius = 180.0;   // eye-level visibility in a dense district is ~150 m
constexpr double kVisitorKeep = 320.0;   // visitors further away are recycled

double segDist(const rj::nav::Vec2& p, const rj::nav::Vec2& a, const rj::nav::Vec2& b) {
  const double vx = b.x - a.x, vy = b.y - a.y;
  const double l2 = vx * vx + vy * vy;
  double t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

double wrapAngle(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

void styleFor(Walker& w, uint64_t seed, int age) {
  static const Color shirts[] = {{235, 235, 232, 255}, {40, 52, 84, 255},  {30, 30, 32, 255},   {128, 130, 134, 255},
                                 {196, 180, 150, 255}, {150, 180, 210, 255}, {96, 104, 72, 255}, {118, 40, 44, 255},
                                 {60, 60, 64, 255},    {210, 206, 196, 255}, {70, 80, 60, 255},  {176, 150, 128, 255}};
  static const Color pants[] = {{28, 28, 30, 255}, {38, 46, 72, 255}, {96, 96, 100, 255}, {170, 150, 120, 255}, {60, 80, 120, 255},
                                {44, 40, 38, 255}};
  static const Color skins[] = {{236, 204, 176, 255}, {222, 186, 150, 255}, {204, 166, 132, 255}};
  static const Color hairs[] = {{22, 18, 16, 255}, {30, 24, 20, 255}, {58, 40, 28, 255}, {92, 66, 44, 255}, {150, 150, 150, 255}};
  rj::sim::Rng r(seed ^ 0x5eedULL);
  w.shirt = shirts[r.next() % 12];
  w.pants = pants[r.next() % 6];
  w.skin = skins[r.next() % 3];
  w.hair = age > 62 ? hairs[4] : hairs[r.next() % 4];
  w.variant = static_cast<int>(r.next() % 3);
  w.age = age;
  w.height_scale = age < 13 ? 0.78f : age < 16 ? 0.92f : static_cast<float>(r.uniform(0.94, 1.06));
  w.phase = static_cast<float>(r.uniform(0.0, 6.28));
}

// Visitor weight of a building: floor area (footprint x storeys) by use (PLATEAU usage code).
double usageFactor(int usage) {
  switch (usage) {
    case 402: case 404: return 1.0;   // commercial, commercial complex
    case 401: return 0.7;             // business (offices)
    case 403: return 0.5;             // hotels
    case 413: case 414: return 0.5;   // shop-houses (ground-floor shops)
    case 421: case 422: return 0.3;   // public, education / welfare
    case 431: return 0.2;             // transport / warehouse
    case 411: case 412: case 415: return 0.04;  // housing (residents are simulated separately)
    default: return 0.1;
  }
}

}  // namespace

double Pedestrians::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 7;
  rng_ ^= rng_ << 17;
  return static_cast<double>(rng_ >> 11) / 9007199254740992.0;
}

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
  n_visitors_ = visitors_pending_ = 0;
  near_at_ = {1e30, 1e30};
  filled_ = false;
}

void Pedestrians::buildNav(const World& world, const Traffic* traffic, const RoadMarkings* markings) {
  clear();
  markings_ = markings;
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
  // Wide carriageways are not crossed mid-block: people cross at the junctions (and at marked
  // crossings, freed below); the junction areas at both ends of each road stay walkable.
  if (traffic)
    for (const auto& e : traffic->edges()) {
      if (e.width < 9.0f || e.pts.size() < 2) continue;
      const double keep = std::max(18.0, e.width * 0.9), r = e.width * 0.5 - 0.3;
      std::vector<double> cum(e.pts.size(), 0.0);
      for (size_t k = 1; k < e.pts.size(); ++k) cum[k] = cum[k - 1] + std::hypot(e.pts[k].x - e.pts[k - 1].x, e.pts[k].y - e.pts[k - 1].y);
      const double L = cum.back();
      if (L < 2.0 * keep + 4.0) continue;
      for (size_t k = 1; k < e.pts.size(); ++k) {
        const double s0 = std::max(cum[k - 1], keep), s1 = std::min(cum[k], L - keep);
        if (s1 <= s0) continue;
        const double seg = std::max(1e-6, cum[k] - cum[k - 1]);
        auto at = [&](double s) {
          const double t = (s - cum[k - 1]) / seg;
          return rj::nav::Vec2{e.pts[k - 1].x + (e.pts[k].x - e.pts[k - 1].x) * t, e.pts[k - 1].y + (e.pts[k].y - e.pts[k - 1].y) * t};
        };
        nav_.blockSegment(at(s0), at(s1), r);
      }
    }
  if (markings)  // marked crossings are always passable (also mid-block ones on real roads)
    for (const auto& xc : markings->crossings())
      for (const auto& poly : xc.polys)
        for (size_t ci : nav_.cellsInPolygon(poly)) nav_.setBlocked(static_cast<int>(ci % nav_.width()), static_cast<int>(ci / nav_.width()), false);
  nav_.computeComponents();
  // Carriageways (real LOD2 road areas via the road graph) are dear to walk on; narrow streets
  // without sidewalks are shared space in Japan and cost only a little more than a sidewalk.
  if (traffic)
    for (const auto& e : traffic->edges()) {
      // (wide roads at the grid's maximum so people walk to the crossings rather than jaywalk)
      const uint8_t c = e.width >= 9.0f ? 255 : e.width >= 6.5f ? 90 : 13;
      for (size_t k = 1; k < e.pts.size(); ++k)
        nav_.costSegment({e.pts[k - 1].x, e.pts[k - 1].y}, {e.pts[k].x, e.pts[k].y}, e.width * 0.5, c);
    }
  cross_id_.clear();
  if (markings) {
    cross_id_.assign(static_cast<size_t>(nav_.width()) * static_cast<size_t>(nav_.height()), -1);
    const auto& xs = markings->crossings();
    for (size_t i = 0; i < xs.size() && i < 32000; ++i)
      for (const auto& poly : xs[i].polys) {
        nav_.costPolygon(poly, rj::nav::GridNav::kBaseCost);
        for (size_t ci : nav_.cellsInPolygon(poly)) cross_id_[ci] = static_cast<int16_t>(i);
      }
  }
  buildSources(world);
  if (std::getenv("RJ_DEBUG")) {  // dump the walkability grid for inspection
    Image img = GenImageColor(nav_.width(), nav_.height(), WHITE);
    size_t nblocked = 0;
    for (int y = 0; y < nav_.height(); ++y)
      for (int x = 0; x < nav_.width(); ++x) {
        const size_t i = static_cast<size_t>(y) * nav_.width() + x;
        Color c = WHITE;
        if (nav_.blocked(x, y)) {
          c = BLACK;
          ++nblocked;
        } else if (!cross_id_.empty() && cross_id_[i] >= 0) {
          c = Color{0, 160, 255, 255};
        } else if (nav_.cost(x, y) > rj::nav::GridNav::kBaseCost) {
          c = nav_.cost(x, y) > 20 ? Color{120, 120, 120, 255} : Color{200, 200, 200, 255};
        }
        ImageDrawPixel(&img, x, nav_.height() - 1 - y, c);
      }
    for (const auto& s : sources_) {
      int cx, cy;
      if (nav_.toCell(s.door, cx, cy)) ImageDrawPixel(&img, cx, nav_.height() - 1 - cy, s.station ? RED : ORANGE);
    }
    ExportImage(img, "nav_debug.png");
    UnloadImage(img);
    TraceLog(LOG_DEBUG, "RJ: nav grid %dx%d, blocked %zu, sources %zu", nav_.width(), nav_.height(), nblocked, sources_.size());
  }
}

void Pedestrians::buildSources(const World& world) {
  sources_.clear();
  auto freeAt = [&](const rj::nav::Vec2& p) {
    int cx, cy;
    return nav_.toCell(p, cx, cy) && nav_.inMainComponent(cx, cy);
  };
  auto dearAt = [&](const rj::nav::Vec2& p) {
    int cx, cy;
    return nav_.toCell(p, cx, cy) && nav_.cost(cx, cy) > rj::nav::GridNav::kBaseCost;
  };
  // Real station entrances (verified underground spaces) and station buildings (POI names).
  for (const auto& im : world.meta().interiors)
    for (const auto& g : im.entrances) {
      const auto p = world.toLocal(g);
      if (auto f = nav_.nearestFree({p.x, p.y}, 6)) sources_.push_back({*f, 25000.0f, true});
    }
  for (const auto& poi : world.meta().pois)
    if (poi.usage == 431 && poi.name.find("駅") != std::string::npos) {
      const auto p = world.toLocal({poi.lat, poi.lon, 0.0});
      if (auto f = nav_.nearestFree({p.x, p.y}, 80)) sources_.push_back({*f, 120000.0f, true});
    }
  // Buildings: door on the street-facing side (free just outside, open space or a road beyond).
  for (const auto& [code, c] : world.cells()) {
    if (!c->cpu) continue;
    const auto& bs = c->cpu->buildings;
    for (size_t b = 0; b < bs.size() && b < c->fp.size(); ++b) {
      const auto& fp = c->fp[b];
      if (fp.size() < 3) continue;
      double area = 0;
      for (size_t i = 0, j = fp.size() - 1; i < fp.size(); j = i++) area += (fp[j].x * fp[i].y - fp[i].x * fp[j].y) * 0.5;
      const double orient = area >= 0 ? 1.0 : -1.0;
      area = std::fabs(area);
      const BuildingInfo& bi = bs[b];
      int storeys = bi.storeys_above > 0 ? bi.storeys_above : 0;
      if (storeys <= 0) {
        const float h = bi.measured_height > 0 ? bi.measured_height : bi.bmax[2] - bi.bmin[2];
        storeys = std::clamp(static_cast<int>(std::lround(h / 3.6f)), 1, 60);
      }
      const double w = std::min(60000.0, area * storeys) * usageFactor(bi.usage);
      if (w < 30.0) continue;
      double best = -1.0;
      rj::nav::Vec2 door{};
      for (size_t i = 0; i < fp.size(); ++i) {
        const Vector2 a = fp[i], bb = fp[(i + 1) % fp.size()];
        const double dx = bb.x - a.x, dy = bb.y - a.y, len = std::hypot(dx, dy);
        if (len < 2.5) continue;
        const rj::nav::Vec2 n{orient * dy / len, -orient * dx / len};  // outward normal
        const rj::nav::Vec2 m{(a.x + bb.x) * 0.5, (a.y + bb.y) * 0.5};
        const rj::nav::Vec2 q1{m.x + n.x * 1.6, m.y + n.y * 1.6}, q2{m.x + n.x * 6.0, m.y + n.y * 6.0};
        if (!freeAt(q1)) continue;
        const double score = (freeAt(q2) ? 1.0 : 0.0) + (dearAt(q2) || dearAt({m.x + n.x * 9.0, m.y + n.y * 9.0}) ? 1.5 : 0.0) +
                             std::min(len, 20.0) / 20.0;
        if (score > best) {
          best = score;
          door = q1;
        }
      }
      if (best >= 0) sources_.push_back({door, static_cast<float>(w), false});
    }
  }
  TraceLog(LOG_INFO, "RJ: visitor sources %zu", sources_.size());
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
    for (const auto& j : batch) out.push_back({j, nav->findPath(j.from, j.to, 250000)});
    return out;
  });
}

void Pedestrians::markCrossings(Walker& w) const {
  w.xings.clear();
  w.next_x = 0;
  if (cross_id_.empty()) return;
  int prev = -1;
  for (double d = 0; d <= w.length; d += 0.5) {
    const auto p = rj::nav::GridNav::pointAt(w.path, d);
    int cx, cy, id = -1;
    if (nav_.toCell(p, cx, cy)) id = cross_id_[static_cast<size_t>(cy) * nav_.width() + cx];
    if (id >= 0 && id != prev) w.xings.push_back({static_cast<float>(d), id});
    prev = id;
  }
}

void Pedestrians::spawnVisitors(const rj::nav::Vec2& me, int hour, float crowd_factor, float real_dt) {
  if (sources_.empty()) return;
  const double moved = std::hypot(me.x - near_at_.x, me.y - near_at_.y);
  if (moved > 25.0) {
    if (moved > 300.0) filled_ = false;  // teleport / first frame: fill along routes again
    near_src_.clear();
    near_cum_.clear();
    near_at_ = me;
    double acc = 0, act = 0;
    for (size_t i = 0; i < sources_.size(); ++i) {
      const double d = std::hypot(sources_[i].door.x - me.x, sources_[i].door.y - me.y);
      if (d > 750.0) continue;
      near_src_.push_back(static_cast<int>(i));
      acc += sources_[i].weight;
      near_cum_.push_back(static_cast<float>(acc));
      if (d < kActivityRadius) act += sources_[i].weight;
    }
    activity_ = static_cast<float>(std::clamp(act / kActivityRef, 0.05, 1.0));
    TraceLog(LOG_DEBUG, "RJ: visitors activity %.2f (weighted floor area %.0f m2 within %.0f m)", activity_, act, kActivityRadius);
  }
  if (near_src_.empty()) return;
  const size_t target =
      std::min(kMaxVisitors, static_cast<size_t>(kVisitorsByHour[std::clamp(hour, 0, 23)] * activity_ * crowd_factor));
  const size_t have = n_visitors_ + visitors_pending_;
  if (have >= target) {
    filled_ = true;
    return;
  }
  size_t n = 0;
  if (!filled_) {
    n = target - have;
    filled_ = true;
  } else {
    spawn_acc_ += real_dt * std::max(1.0f, static_cast<float>(target) / 60.0f);
    n = std::min(target - have, static_cast<size_t>(spawn_acc_));
    spawn_acc_ -= static_cast<float>(static_cast<int>(spawn_acc_));
  }
  const bool mid = n > 8;  // bulk fill: start somewhere along the route
  auto pick = [&]() {
    const float r = static_cast<float>(rnd()) * near_cum_.back();
    const size_t k = static_cast<size_t>(std::lower_bound(near_cum_.begin(), near_cum_.end(), r) - near_cum_.begin());
    return near_src_[std::min(k, near_src_.size() - 1)];
  };
  for (size_t s = 0; s < n; ++s) {
    int o = -1, d = -1;
    for (int t = 0; t < 16 && o < 0; ++t) {
      const int c = pick();
      if (std::hypot(sources_[static_cast<size_t>(c)].door.x - me.x, sources_[static_cast<size_t>(c)].door.y - me.y) < kSpawnRadius) o = c;
    }
    if (o < 0) continue;
    const auto& O = sources_[static_cast<size_t>(o)];
    for (int t = 0; t < 16 && d < 0; ++t) {
      const int c = pick();
      const auto& D = sources_[static_cast<size_t>(c)];
      const double dd = std::hypot(D.door.x - O.door.x, D.door.y - O.door.y);
      if (c != o && dd > 40.0 && dd < 420.0 && !(O.station && D.station) &&
          std::hypot(D.door.x - me.x, D.door.y - me.y) < kVisitorKeep)
        d = c;
    }
    if (d < 0) continue;
    Job j;
    j.npc = next_visitor_++;
    j.from = O.door;
    j.to = sources_[static_cast<size_t>(d)].door;
    j.visitor = true;
    j.mid_route = mid;
    queue_.push_back(j);
    ++visitors_pending_;
  }
}

void Pedestrians::update(TownSim& town, const World& world, const TrafficSignals& signals, const rj::sim::CivilDateTime& now,
                         const rj::geo::Vec3d& player, float real_dt, float crowd_factor) {
  if (!nav_.valid()) return;
  const rj::nav::Vec2 me{player.x, player.y};
  const int minute = now.minuteOfDay();
  auto local = [&](const rj::sim::PlaceRef& p) {
    const auto v = world.toLocal({p.pos.lat_deg, p.pos.lon_deg, 0.0});
    return rj::nav::Vec2{v.x, v.y};
  };

  // Residents: re-evaluate who is walking once per game minute.
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
    size_t n60 = 0, n120 = 0, nwait = 0;
    for (const auto& [id, w] : walkers_) {
      const double d = std::hypot(w.pos.x - me.x, w.pos.y - me.y);
      n60 += d < 60 ? 1 : 0;
      n120 += d < 120 ? 1 : 0;
      nwait += w.waiting ? 1 : 0;
    }
    TraceLog(LOG_DEBUG, "RJ: peds minute=%d trips=%zu near=%zu walkers=%zu (visitors %zu, <60m %zu, <120m %zu, waiting %zu) pending=%zu",
             minute, trips_.size(), cands.size(), walkers_.size(), n_visitors_, n60, n120, nwait, pending_.size());
    if (cands.size() > kMaxWalkers) cands.resize(kMaxWalkers);
    for (const auto& c : cands) {
      const size_t id = c.t.npc;
      if (walkers_.count(id) || (pending_.count(id) && pending_[id] == c.t.start_min)) continue;
      if (failed_.count(id) && failed_[id] == c.t.start_min) continue;
      pending_[id] = c.t.start_min;
      Job j;
      j.npc = id;
      j.trip = c.t;
      j.from = c.a;
      j.to = c.b;
      queue_.push_back(j);
    }
  }
  spawnVisitors(me, minute / 60, crowd_factor, real_dt);

  // Collect finished routes.
  if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto results = job_.get();
    for (auto& r : results) {
      const Job& j = r.job;
      if (j.visitor) {
        if (visitors_pending_ > 0) --visitors_pending_;
      } else {
        pending_.erase(j.npc);
      }
      if (!r.path || r.path->size() < 2) {
        if (!j.visitor) failed_[j.npc] = j.trip.start_min;
        continue;
      }
      Walker w;
      w.npc = j.npc;
      w.visitor = j.visitor;
      w.trip = j.trip;
      w.path = std::move(*r.path);
      w.length = rj::nav::GridNav::pathLength(w.path);
      if (j.visitor) {
        if (w.length < 15.0) continue;
        w.dist = j.mid_route ? rnd() * w.length * 0.92 : 0.0;
        const double a = rnd();  // Shibuya visitors skew young
        const int age = a < 0.15 ? 15 + static_cast<int>(rnd() * 5) : a < 0.62 ? 20 + static_cast<int>(rnd() * 15)
                        : a < 0.87 ? 35 + static_cast<int>(rnd() * 20) : 55 + static_cast<int>(rnd() * 20);
        styleFor(w, j.npc * 0x9e3779b97f4a7c15ULL, age);
      } else {
        const double span = std::max(1, j.trip.end_min - j.trip.start_min);
        const double f = std::clamp((minute + now.second / 60.0 - j.trip.start_min) / span, 0.0, 1.0);
        w.dist = f * w.length;
        const auto& n = town.npc(j.npc);
        styleFor(w, n.seed, n.age);
      }
      w.speed = static_cast<float>((w.age > 65 ? 1.1 : 1.3) + rnd() * 0.3);
      w.offset = static_cast<float>((rnd() * 2.0 - 1.0) * 1.3);
      w.off_eff = w.offset;
      w.wait_back = static_cast<float>(0.4 + rnd() * 1.8);
      markCrossings(w);
      const auto p0 = rj::nav::GridNav::pointAt(w.path, w.dist, &w.dir);
      w.pos = p0;
      w.yaw = static_cast<float>(std::atan2(w.dir.x, w.dir.y));
      walkers_[j.npc] = std::move(w);
    }
    job_ = {};
  }
  startJobs();

  // Advance at a real walking pace; wait at red pedestrian signals; drop arrivals and far walkers.
  n_visitors_ = 0;
  for (auto it = walkers_.begin(); it != walkers_.end();) {
    Walker& w = it->second;
    double step = w.speed * real_dt;
    w.waiting = false;
    while (w.next_x < w.xings.size() && w.dist >= w.xings[w.next_x].first) ++w.next_x;  // on / past it
    if (markings_ && w.next_x < w.xings.size()) {
      const auto [de, id] = w.xings[w.next_x];
      const Crossing& c = markings_->crossings()[static_cast<size_t>(id)];
      if (c.group >= 0) {
        const double hold_at = de - w.wait_back;
        if (w.dist + step >= hold_at && signals.pedestrian(c.group, c.phase) != PedLamp::Walk) {
          step = std::max(0.0, hold_at - w.dist);
          w.waiting = w.dist >= hold_at - 0.05;
        }
      }
    }
    // the player's car: stop if it is coming at you, step aside, never be driven through
    bool dodging = false;
    if (hz_on_) {
      const double fx = std::sin(hz_yaw_), fy = std::cos(hz_yaw_), rx = fy, ry = -fx;
      const double dx = w.pos.x - hz_.x, dy = w.pos.y - hz_.y;
      const double along = dx * fx + dy * fy, lat = dx * rx + dy * ry;
      const double side = lat >= 0 ? 1.0 : -1.0;
      int wx, wy;
      const bool on_road = nav_.toCell(w.pos, wx, wy) && nav_.cost(wx, wy) > 20;  // (people on a crossing hurry on)
      if (std::fabs(lat) < 2.2 && along > -3.0 && along < 4.0 + hz_v_ * 1.3 && (hz_v_ > 0.4 || (std::fabs(along) < 2.6 && std::fabs(lat) < 1.3))) {
        step = on_road ? step * 1.6 : 0.0;
        dodging = true;
        const double push = std::min(1.0, 2.6 * real_dt);
        w.dodge.x += rx * side * push;
        w.dodge.y += ry * side * push;
        if (std::fabs(along) < 2.5 && std::fabs(lat) < 1.15) {  // inside the car's outline: out of the way at once
          w.dodge.x += rx * side * (1.2 - std::fabs(lat));
          w.dodge.y += ry * side * (1.2 - std::fabs(lat));
        }
      }
    }
    if (!dodging) {
      const float k = std::max(0.0f, 1.0f - 0.4f * real_dt);
      w.dodge.x *= k;
      w.dodge.y *= k;
    }
    bool held = false;
    if (auto h = hold_.find(it->first); h != hold_.end()) {  // talking to the player: stand, face them
      held = true;
      step = 0.0;
      w.waiting = true;
      if ((h->second -= real_dt) <= 0.0f) hold_.erase(h);
    }
    w.dist += step;
    if (!w.waiting && !dodging) w.phase += real_dt * 7.5f * (w.speed / 1.4f);
    const auto base = rj::nav::GridNav::pointAt(w.path, w.dist, &w.dir);
    // Spread across the sidewalk: lateral offset, pulled in where it would leave the walkable area.
    int bx, by, ox, oy;
    const rj::nav::Vec2 perp{w.dir.y, -w.dir.x};
    const rj::nav::Vec2 cand{base.x + perp.x * w.offset, base.y + perp.y * w.offset};
    const bool ok = nav_.toCell(base, bx, by) && nav_.toCell(cand, ox, oy) && !nav_.blocked(ox, oy) &&
                    nav_.cost(ox, oy) <= nav_.cost(bx, by);
    const float tgt = ok ? w.offset : 0.0f;
    const float rate = 0.9f * real_dt;
    w.off_eff += std::clamp(tgt - w.off_eff, -rate, rate);
    w.pos = {base.x + perp.x * w.off_eff + w.dodge.x, base.y + perp.y * w.off_eff + w.dodge.y};
    const double hy = held ? std::atan2(hold_face_.x - w.pos.x, hold_face_.y - w.pos.y) : std::atan2(w.dir.x, w.dir.y);
    w.yaw = static_cast<float>(wrapAngle(w.yaw + wrapAngle(hy - w.yaw) * std::min(1.0, real_dt * 5.0)));
    if (auto h = world.surfaceHeight(w.pos.x, w.pos.y)) w.z = static_cast<float>(*h);
    const bool far = std::hypot(w.pos.x - me.x, w.pos.y - me.y) > (w.visitor ? kVisitorKeep : kVisibleRadius * 1.3);
    if (w.dist >= w.length || far) {
      it = walkers_.erase(it);
    } else {
      n_visitors_ += w.visitor ? 1 : 0;
      ++it;
    }
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
