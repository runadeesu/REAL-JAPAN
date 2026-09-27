#include "game/jobs.hpp"

#include <algorithm>
#include <cmath>

#include "game/traffic.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {
using V3 = rj::geo::Vec3d;
double dist2(const V3& a, const V3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool isShop(int u) { return u == 402 || u == 404 || u == 413 || u == 414; }
bool isHome(int u) { return u == 411 || u == 412 || u == 413 || u == 414 || u == 415; }
}  // namespace

uint32_t Jobs::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

double Jobs::distanceTo(const V3& p) const { return dist2(p, target().pos); }

bool Jobs::kerbNear(const Traffic& traffic, const V3& q, V3& out) const {
  // nearest carriageway point, then out to the kerb on q's side
  double best = 1e30;
  V3 bp{}, bn{};
  float bw = 6;
  for (const auto& e : traffic.edges())
    for (size_t k = 1; k < e.pts.size(); ++k) {
      const auto& A = e.pts[k - 1];
      const auto& B = e.pts[k];
      const double vx = B.x - A.x, vy = B.y - A.y, l2 = vx * vx + vy * vy;
      if (l2 < 1e-6) continue;
      const double t = std::clamp(((q.x - A.x) * vx + (q.y - A.y) * vy) / l2, 0.0, 1.0);
      const V3 p{A.x + vx * t, A.y + vy * t, A.z + (B.z - A.z) * t};
      const double d = dist2(p, q);
      if (d < best) {
        best = d;
        bp = p;
        const double l = std::sqrt(l2);
        bn = {-vy / l, vx / l, 0};
        bw = e.width;
      }
    }
  if (best > 250.0) return false;
  const double side = (q.x - bp.x) * bn.x + (q.y - bp.y) * bn.y >= 0 ? 1.0 : -1.0;
  out = {bp.x + bn.x * side * (bw * 0.5 + 1.0), bp.y + bn.y * side * (bw * 0.5 + 1.0), bp.z};
  return true;
}

bool Jobs::randomBuilding(const World& world, const V3& from, double rmin, double rmax, bool shop, JobPlace& out) {
  struct Cand {
    const LoadedCell* c;
    size_t i;
  };
  std::vector<Cand> cands;
  for (const auto& [id, c] : world.cells()) {
    if (!c->cpu) continue;
    const auto& bs = c->cpu->buildings;
    for (size_t i = 0; i < bs.size() && i < c->fp.size(); ++i) {
      if (shop ? !isShop(bs[i].usage) : !isHome(bs[i].usage)) continue;
      const auto& fp = c->fp[i];
      if (fp.empty()) continue;
      const double d = std::hypot(fp[0].x - from.x, fp[0].y - from.y);
      if (d >= rmin && d <= rmax) cands.push_back({c.get(), i});
    }
  }
  if (cands.empty()) return false;
  const Cand& k = cands[rnd() % cands.size()];
  const auto& fp = k.c->fp[k.i];
  // front door: the middle of the footprint edge facing the nearest street
  double cx = 0, cy = 0;
  for (const auto& p : fp) cx += p.x, cy += p.y;
  cx /= fp.size();
  cy /= fp.size();
  out.name = k.c->cpu->buildings[k.i].name;
  out.usage = k.c->cpu->buildings[k.i].usage;
  out.pos = {cx, cy, k.c->cpu->buildings[k.i].ground_z};
  if (auto h = world.terrainHeight(cx, cy)) out.pos.z = *h;
  return true;
}

bool Jobs::pickTaxiRide(const World& world, const Traffic& traffic, const V3& from) {
  // someone waiting at the kerb 150-500 m away
  for (int tries = 0; tries < 40; ++tries) {
    const auto& es = traffic.edges();
    if (es.empty()) return false;
    const auto& e = es[rnd() % es.size()];
    if (e.pts.size() < 2 || e.width < 5.5f) continue;
    const auto& m = e.pts[e.pts.size() / 2];
    const double d = dist2(m, from);
    if (d < 150.0 || d > 500.0) continue;
    V3 kerb;
    const V3 side{m.x + 30.0 * ((rnd() & 1) ? 1 : -1), m.y, m.z};
    if (!kerbNear(traffic, side, kerb)) continue;
    pickup_ = {"", 0, kerb};
    break;
  }
  if (pickup_.pos.x == 0 && pickup_.pos.y == 0) return false;
  // destination: a named place (POI) or a building 0.8-3 km away
  std::vector<JobPlace> dests;
  for (const auto& p : world.meta().pois) {
    const V3 q = world.toLocal({p.lat, p.lon, 0.0});
    const double d = dist2(q, pickup_.pos);
    if (d > 800.0 && d < 3500.0 && !p.name.empty()) dests.push_back({p.name, p.usage, q});
  }
  JobPlace b;
  if ((dests.empty() || rnd() % 3 == 0) && randomBuilding(world, pickup_.pos, 800.0, 3000.0, (rnd() & 1) != 0, b)) dests.push_back(b);
  if (dests.empty()) return false;
  dropoff_ = dests[rnd() % dests.size()];
  V3 kerb;
  if (kerbNear(traffic, dropoff_.pos, kerb)) dropoff_.pos = kerb;
  return true;
}

bool Jobs::startTaxi(const World& world, const Traffic& traffic, const V3& from) {
  rng_ ^= static_cast<uint32_t>(from.x * 131.0 + from.y * 71.0) | 1u;
  pickup_ = {};
  if (!pickTaxiRide(world, traffic, from)) return false;
  kind_ = JobKind::Taxi;
  stage_ = 0;
  meter_m_ = meter_slow_s_ = 0;
  last_ok_ = false;
  stopped_s_ = 0;
  return true;
}

bool Jobs::startDelivery(const World& world, const Traffic& traffic, const V3& from) {
  rng_ ^= static_cast<uint32_t>(from.x * 97.0 + from.y * 53.0) | 1u;
  JobPlace shop, home;
  if (!randomBuilding(world, from, 120.0, 700.0, true, shop)) return false;
  if (!randomBuilding(world, shop.pos, 300.0, 1400.0, false, home)) return false;
  (void)traffic;
  pickup_ = shop;
  dropoff_ = home;
  kind_ = JobKind::Delivery;
  stage_ = 0;
  elapsed_ = 0;
  trip_m_ = dist2(shop.pos, home.pos);
  limit_ = 0;
  return true;
}

int64_t Jobs::meterYen(int hour) const {
  // Tokyo special wards (2022 tariff): first 1,096 m 500 yen, then 100 yen per 255 m;
  // time-and-distance: 100 yen per 1 min 35 s at or below 10 km/h. Late night (22-5 h) +20 %.
  int64_t f = 500;
  if (meter_m_ > 1096.0) f += 100 * static_cast<int64_t>(std::ceil((meter_m_ - 1096.0) / 255.0));
  f += 100 * static_cast<int64_t>(meter_slow_s_ / 95.0);
  if (hour >= 22 || hour < 5) f = static_cast<int64_t>(std::llround(f * 1.2 / 10.0) * 10);
  return f;
}

std::vector<JobEvent> Jobs::update(double dt, const World& world, const Traffic& traffic, const V3& player, double speed_ms, bool in_taxi,
                                   bool in_vehicle, int hour) {
  std::vector<JobEvent> ev;
  if (!active()) return ev;
  const double d = distanceTo(player);
  if (kind_ == JobKind::Taxi) {
    if (!in_taxi) {
      ev.push_back({"job.taxi.stopped", {}, 0});
      kind_ = JobKind::None;
      return ev;
    }
    stopped_s_ = speed_ms < 0.6 ? stopped_s_ + dt : 0.0;
    if (stage_ == 1) {
      if (last_ok_) meter_m_ += dist2(player, last_);
      if (speed_ms < 10.0 / 3.6) meter_slow_s_ += dt;
    }
    last_ = player;
    last_ok_ = true;
    if (stage_ == 0 && d < 12.0 && stopped_s_ > 1.0) {
      stage_ = 1;
      meter_m_ = meter_slow_s_ = 0;
      ev.push_back({"job.taxi.picked", {{"dest", dropoff_.name.empty() ? std::string("-") : dropoff_.name}}, 0});
    } else if (stage_ == 1 && d < 18.0 && stopped_s_ > 1.0) {
      const int64_t fare = meterYen(hour);
      const int64_t share = fare * 50 / 100;  // driver's share (game assumption)
      earned_ += share;
      ++count_;
      ev.push_back({"job.taxi.paid", {{"fare", std::to_string(fare)}, {"pay", std::to_string(share)}}, share});
      pickup_ = {};
      if (!pickTaxiRide(world, traffic, player)) {
        kind_ = JobKind::None;
        ev.push_back({"job.taxi.none", {}, 0});
      } else {
        stage_ = 0;
      }
    }
    return ev;
  }
  if (kind_ == JobKind::Delivery) {
    elapsed_ += dt;
    const double reach = in_vehicle ? 16.0 : 8.0;
    if (stage_ == 0 && d < reach) {
      stage_ = 1;
      // time allowed: about 3.5 m/s along the way plus handling (game value)
      limit_ = elapsed_ + 120.0 + trip_m_ * 1.35 / 3.5;
      ev.push_back({"job.delivery.picked",
                    {{"shop", pickup_.name.empty() ? std::string("-") : pickup_.name}, {"min", std::to_string(static_cast<int>((limit_ - elapsed_) / 60.0 + 0.5))}},
                    0});
    } else if (stage_ == 1 && d < reach) {
      int64_t pay = 300 + static_cast<int64_t>(trip_m_ * 0.12);
      const bool late = elapsed_ > limit_;
      if (late) pay = pay * 7 / 10;
      pay = pay / 10 * 10;
      earned_ += pay;
      ++count_;
      ev.push_back({late ? "job.delivery.late" : "job.delivery.done", {{"pay", std::to_string(pay)}}, pay});
      JobPlace shop, home;
      if (randomBuilding(world, player, 120.0, 700.0, true, shop) && randomBuilding(world, shop.pos, 300.0, 1400.0, false, home)) {
        pickup_ = shop;
        dropoff_ = home;
        trip_m_ = dist2(shop.pos, home.pos);
        stage_ = 0;
      } else {
        kind_ = JobKind::None;
      }
    }
  }
  return ev;
}

}  // namespace rjc
