#include "world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "platform/paths.hpp"
#include "util/text.hpp"
#include "world/canopy.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

int64_t bucketKey(int bx, int by) { return (static_cast<int64_t>(bx) << 32) ^ static_cast<uint32_t>(by); }

bool pointInPoly(const std::vector<Vector2>& poly, float x, float y) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vector2 a = poly[i], b = poly[j];
    if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) in = !in;
  }
  return in;
}

// Closest point on the polygon boundary.
Vector2 closestOnPoly(const std::vector<Vector2>& poly, Vector2 p, float& dist) {
  Vector2 best{};
  float bd = 1e30f;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vector2 a = poly[j], b = poly[i];
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 1e-9f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vector2 q{a.x + t * vx, a.y + t * vy};
    const float d = std::hypot(p.x - q.x, p.y - q.y);
    if (d < bd) {
      bd = d;
      best = q;
    }
  }
  dist = bd;
  return best;
}

}  // namespace

World::World() = default;

World::~World() { unloadAll(); }

bool World::loadMeta(const std::filesystem::path& dir, std::string& err) {
  dir_ = dir;
  auto t = readText(dir / "client.txt");
  if (!t) {
    err = "missing " + pathToUtf8(dir / "client.txt");
    return false;
  }
  std::istringstream in(*t);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto head = splitWs(line, 2);
    if (head.size() < 2) continue;
    const std::string& k = head[0];
    try {
      if (k == "name_ja") meta_.name_ja = head[1];
      else if (k == "name_en") meta_.name_en = head[1];
      else if (k == "world") meta_.fictional = head.size() > 1 && head[1] == "fictional";
      else if (k == "spawn") {
        const auto v = splitWs(head[1]);
        meta_.spawn_lat = std::stod(v.at(0));
        meta_.spawn_lon = std::stod(v.at(1));
        meta_.spawn_heading = std::stod(v.at(2));
      } else if (k == "core_bbox") {
        const auto v = splitWs(head[1]);
        for (int i = 0; i < 4; ++i) meta_.core_bbox[i] = std::stod(v.at(static_cast<size_t>(i)));
      } else if (k == "cell") {
        const auto v = splitWs(head[1]);
        meta_.cells.push_back({v.at(0), std::stod(v.at(1)), std::stod(v.at(2)), std::stod(v.at(3))});
      } else if (k == "source") {
        const auto v = splitWs(head[1], 3);
        meta_.sources.push_back({v.at(1), v.size() > 2 ? v[2] : ""});
      } else if (k == "interior") {
        const auto v = splitWs(head[1], 5);
        InteriorMeta im;
        im.id = v.at(0);
        im.file = v.at(1);
        im.source_index = std::stoi(v.at(2));
        im.status = v.at(3);
        im.name = v.size() > 4 ? v[4] : "";
        meta_.interiors.push_back(im);
      } else if (k == "entrance") {
        const auto v = splitWs(head[1]);
        for (auto& im : meta_.interiors)
          if (im.id == v.at(0)) im.entrances.push_back({std::stod(v.at(1)), std::stod(v.at(2)), std::stod(v.at(3))});
      } else if (k == "opening") {
        const auto v = splitWs(head[1]);
        std::vector<rj::geo::Geodetic> ring;
        for (size_t i = 1; i < v.size(); ++i) {
          double la = 0, lo = 0;
          if (std::sscanf(v[i].c_str(), "%lf,%lf", &la, &lo) == 2) ring.push_back({la, lo, 0.0});
        }
        for (auto& im : meta_.interiors)
          if (im.id == v.at(0) && ring.size() >= 3) im.openings.push_back(std::move(ring));
      } else if (k == "place") {  // place <lat> <lon> <city|village> <name>|<english>
        const auto v = splitWs(head[1], 4);
        Place pl;
        pl.lat = std::stod(v.at(0));
        pl.lon = std::stod(v.at(1));
        pl.city = v.at(2) == "city";
        const std::string names = v.size() > 3 ? v[3] : "";
        const auto bar = names.find('|');
        pl.name = names.substr(0, bar);
        if (bar != std::string::npos) pl.name_en = names.substr(bar + 1);
        meta_.places.push_back(pl);
      } else if (k == "reading") {
        // (station / place readings: read by Trains from rail.txt)
      } else if (k == "poi") {
        const auto v = splitWs(head[1], 5);
        meta_.pois.push_back({std::stod(v.at(0)), std::stod(v.at(1)), std::stoi(v.at(2)), std::stof(v.at(3)),
                              v.size() > 4 ? v[4] : ""});
      }
    } catch (...) {
      err = "bad line in client.txt: " + line;
      return false;
    }
  }
  if (meta_.cells.empty()) {
    err = "slice has no cells";
    return false;
  }
  return true;
}

void World::resetOrigin(const rj::geo::Geodetic& g) {
  unloadAll();
  origin_ = std::make_unique<rj::geo::FloatingOrigin>(g, 2000.0);
  rj::stream::LayerConfig city{"city", 3, 1800.0, 2600.0, 6.0, 0.0, 64u << 20};
  streamer_ = std::make_unique<rj::stream::HierarchicalStreamer>(std::vector<rj::stream::LayerConfig>{city},
                                                                 size_t{3} << 30, 2, *this);
  current_view_ = 0;
  interior_streamer_ = std::make_unique<rj::stream::InteriorStreamer>(250.0, 350.0, size_t{512} << 20, *this);
  interior_candidates_.clear();
  for (const auto& im : meta_.interiors) {
    if (im.entrances.empty()) continue;
    rj::stream::InteriorCandidate c;
    c.building_id = im.id;
    c.exterior_cell = *rj::geo::MeshCode::fromLatLon({im.entrances[0].lat_deg, im.entrances[0].lon_deg}, 3);
    for (const auto& e : im.entrances) c.entrances.push_back({e.lat_deg, e.lon_deg});
    c.interior_bytes = 16u << 20;
    interior_candidates_.push_back(c);
  }
}

void World::loadInterior(const std::string& id) {
  if (interiors_.count(id)) return;
  for (const auto& im : meta_.interiors) {
    if (im.id != id) continue;
    auto in = std::make_unique<Interior>();
    std::string err;
    if (!in->load(dir_ / "interiors" / im.file, err)) {
      TraceLog(LOG_WARNING, "RJ: interior %s: %s", id.c_str(), err.c_str());
      return;
    }
    in->place(origin_->frame());
    interiors_[id] = std::move(in);
  }
}

void World::unloadInterior(const std::string& id) {
  auto it = interiors_.find(id);
  if (it == interiors_.end()) return;
  it->second->unload();
  interiors_.erase(it);
}

bool World::forceLoadInterior(const std::string& id) {
  loadInterior(id);
  return interiors_.count(id) > 0;
}

const Interior* World::interior(const std::string& id) const {
  auto it = interiors_.find(id);
  return it == interiors_.end() ? nullptr : it->second.get();
}

void World::requestLoad(const rj::stream::StreamKey& key) {
  const std::string code = key.cell.str();
  const bool known = std::any_of(meta_.cells.begin(), meta_.cells.end(), [&](const CellMeta& m) { return m.mesh == code; });
  if (!known) {
    empty_completions_.push_back(key);  // outside data coverage: resident but empty
    return;
  }
  const std::filesystem::path path = dir_ / "cells" / (code + ".rjcell");
  const std::filesystem::path dpath = dir_ / "cells" / (code + ".rjdet");
  Job j;
  j.key = key;
  j.fut = std::async(std::launch::async, [path, dpath]() -> std::unique_ptr<CellCpu> {
    auto data = readFile(path);
    if (!data) return nullptr;
    auto c = std::make_unique<CellCpu>();
    std::string err;
    if (!parseCell(*data, *c, err)) {
      TraceLog(LOG_WARNING, "RJ: cell parse failed: %s", err.c_str());
      return nullptr;
    }
    prepareTerrain(*c);
    if (auto det = readFile(dpath)) {
      if (!parseDetail(*det, c->detail, err)) TraceLog(LOG_WARNING, "RJ: street detail parse failed: %s", err.c_str());
      c->bytes += det->size();
    }
    buildCanopy(*c);  // forest crowns from the land-cover map (fictional country)
    return c;
  });
  jobs_.push_back(std::move(j));
}

void World::requestUnload(const rj::stream::StreamKey& key) {
  const std::string code = key.cell.str();
  for (auto& j : jobs_)
    if (j.key == key) j.cancelled = true;
  auto it = loaded_.find(code);
  if (it != loaded_.end()) {
    unloadDetail(it->second->gpu.detail);
    unloadCell(it->second->gpu);
    loaded_.erase(it);
    rebuildHash();
  }
}

void World::unloadAll() {
  for (auto& j : jobs_)
    if (j.fut.valid()) j.fut.wait();
  jobs_.clear();
  for (auto& [k, c] : loaded_) {
    unloadDetail(c->gpu.detail);
    unloadCell(c->gpu);
  }
  loaded_.clear();
  hash_.clear();
  for (auto& [k, in] : interiors_) in->unload();
  interiors_.clear();
}

void World::placeCell(LoadedCell& c) {
  c.to_origin = origin_->frame().transformFrom(c.frame);
  c.model = rigidToRaylib(c.to_origin);
  const CellCpu& cpu = *c.cpu;
  c.fp.assign(cpu.buildings.size(), {});
  c.zmin.assign(cpu.buildings.size(), 0);
  c.zmax.assign(cpu.buildings.size(), 0);
  for (size_t b = 0; b < cpu.buildings.size(); ++b) {
    const BuildingInfo& bi = cpu.buildings[b];
    auto& poly = c.fp[b];
    for (uint32_t k = 0; k < bi.fp_count; ++k) {
      const size_t idx = (bi.fp_first + k) * 2;
      if (idx + 1 >= cpu.footprints.size()) break;
      const auto p = c.to_origin.apply({cpu.footprints[idx], cpu.footprints[idx + 1], bi.ground_z});
      poly.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
    }
    const double cx = 0.5 * (bi.bmin[0] + bi.bmax[0]), cy = 0.5 * (bi.bmin[1] + bi.bmax[1]);
    c.zmin[b] = static_cast<float>(c.to_origin.apply({cx, cy, bi.ground_z}).z);
    c.zmax[b] = static_cast<float>(c.to_origin.apply({cx, cy, bi.bmax[2]}).z);
  }
  const size_t n = cpu.tpos.size() / 3;
  c.tx.resize(n);
  c.ty.resize(n);
  c.tz.resize(n);
  for (size_t k = 0; k < n; ++k) {
    const auto p = c.to_origin.apply({cpu.tpos[k * 3], cpu.tpos[k * 3 + 1], cpu.tpos[k * 3 + 2]});
    c.tx[k] = static_cast<float>(p.x);
    c.ty[k] = static_cast<float>(p.y);
    c.tz[k] = static_cast<float>(p.z);
  }
  // Street detail -> origin ENU.
  auto toO = [&](const float* v) { return c.to_origin.apply({v[0], v[1], v[2]}); };
  const auto& det = cpu.detail;
  c.walk.resize(det.walk.size());
  c.walk_hash.clear();
  for (size_t t = 0; t + 8 < det.walk.size(); t += 9) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 3; ++k) {
      const auto p = toO(&det.walk[t + k * 3]);
      c.walk[t + k * 3] = static_cast<float>(p.x);
      c.walk[t + k * 3 + 1] = static_cast<float>(p.y);
      c.walk[t + k * 3 + 2] = static_cast<float>(p.z);
      x0 = std::min(x0, static_cast<float>(p.x));
      x1 = std::max(x1, static_cast<float>(p.x));
      y0 = std::min(y0, static_cast<float>(p.y));
      y1 = std::max(y1, static_cast<float>(p.y));
    }
    for (int bx = static_cast<int>(std::floor(x0 / 4.0)); bx <= static_cast<int>(std::floor(x1 / 4.0)); ++bx)
      for (int by = static_cast<int>(std::floor(y0 / 4.0)); by <= static_cast<int>(std::floor(y1 / 4.0)); ++by)
        c.walk_hash[bucketKey(bx, by)].push_back(static_cast<uint32_t>(t / 9));
  }
  c.deck.clear();
  c.deck_hash.clear();
  for (size_t t = 0; t + 8 < det.deck.size(); t += 9) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 3; ++k) {
      const auto p = toO(&det.deck[t + k * 3]);
      c.deck.push_back(static_cast<float>(p.x));
      c.deck.push_back(static_cast<float>(p.y));
      c.deck.push_back(static_cast<float>(p.z));
      x0 = std::min(x0, static_cast<float>(p.x));
      x1 = std::max(x1, static_cast<float>(p.x));
      y0 = std::min(y0, static_cast<float>(p.y));
      y1 = std::max(y1, static_cast<float>(p.y));
    }
    for (int bx = static_cast<int>(std::floor(x0 / 4.0)); bx <= static_cast<int>(std::floor(x1 / 4.0)); ++bx)
      for (int by = static_cast<int>(std::floor(y0 / 4.0)); by <= static_cast<int>(std::floor(y1 / 4.0)); ++by)
        c.deck_hash[bucketKey(bx, by)].push_back(static_cast<uint32_t>(t / 9));
  }
  c.cross.resize(det.cross.size());
  for (size_t v = 0; v + 2 < det.cross.size(); v += 3) {
    const auto p = toO(&det.cross[v]);
    c.cross[v] = static_cast<float>(p.x);
    c.cross[v + 1] = static_cast<float>(p.y);
    c.cross[v + 2] = static_cast<float>(p.z);
  }
  c.mark_hash.clear();
  for (size_t v = 0; v + 1 < det.marks.size(); v += 2) {
    const float m[3] = {det.marks[v], det.marks[v + 1], 0.0f};
    const auto p = toO(m);
    c.mark_hash.insert(bucketKey(static_cast<int>(std::floor(p.x / 2.0)), static_cast<int>(std::floor(p.y / 2.0))));
  }
  c.walls.clear();
  c.wall_hash.clear();
  for (size_t w = 0; w + 5 < det.walls.size(); w += 6) {
    const float a[3] = {det.walls[w], det.walls[w + 1], det.walls[w + 4]};
    const float b[3] = {det.walls[w + 2], det.walls[w + 3], det.walls[w + 5]};
    const auto A = toO(a), B = toO(b);
    const size_t i = c.walls.size() / 6;
    c.walls.insert(c.walls.end(), {static_cast<float>(A.x), static_cast<float>(A.y), static_cast<float>(B.x), static_cast<float>(B.y),
                                   static_cast<float>(A.z), static_cast<float>(B.z)});
    const float x0 = static_cast<float>(std::min(A.x, B.x)) - 0.5f, x1 = static_cast<float>(std::max(A.x, B.x)) + 0.5f;
    const float y0 = static_cast<float>(std::min(A.y, B.y)) - 0.5f, y1 = static_cast<float>(std::max(A.y, B.y)) + 0.5f;
    for (int bx = static_cast<int>(std::floor(x0 / 4.0)); bx <= static_cast<int>(std::floor(x1 / 4.0)); ++bx)
      for (int by = static_cast<int>(std::floor(y0 / 4.0)); by <= static_cast<int>(std::floor(y1 / 4.0)); ++by)
        c.wall_hash[bucketKey(bx, by)].push_back(static_cast<uint32_t>(i));
  }
  c.lights.clear();
  for (const auto& l : det.lights) c.lights.push_back({toO(l.pos), l.range, static_cast<int>(l.kind)});
  c.signals.clear();
  for (const auto& sg : det.signals)
    c.signals.push_back({toO(sg.pos), sg.axis_yaw, sg.facing_yaw, sg.length, static_cast<int>(sg.kind), sg.group,
                         static_cast<int>(sg.phase)});

  const int nx = cpu.tnx, ny = cpu.tny;
  if (nx >= 2 && ny >= 2) {
    c.p00 = {c.tx[0], c.ty[0]};
    c.ex = {(c.tx[static_cast<size_t>(nx - 1)] - c.tx[0]) / (nx - 1), (c.ty[static_cast<size_t>(nx - 1)] - c.ty[0]) / (nx - 1)};
    const size_t top = static_cast<size_t>((ny - 1) * nx);
    c.ey = {(c.tx[top] - c.tx[0]) / (ny - 1), (c.ty[top] - c.ty[0]) / (ny - 1)};
  }
}

void World::rebuildHash() {
  hash_.clear();
  for (const auto& [code, c] : loaded_) {
    for (size_t b = 0; b < c->fp.size(); ++b) {
      const auto& poly = c->fp[b];
      if (poly.size() < 3) continue;
      float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
      for (const auto& p : poly) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
      }
      for (int bx = static_cast<int>(std::floor(x0 / kBucket)); bx <= static_cast<int>(std::floor(x1 / kBucket)); ++bx)
        for (int by = static_cast<int>(std::floor(y0 / kBucket)); by <= static_cast<int>(std::floor(y1 / kBucket)); ++by)
          hash_[bucketKey(bx, by)].push_back({c.get(), static_cast<int>(b)});
    }
  }
}

bool World::update(rj::geo::Vec3d& player, double view_distance_m) {
  if (!streamer_) return false;
  bool rebased = false;
  if (origin_ && origin_->update(player)) {
    rebased = true;
    for (auto& [k, c] : loaded_) placeCell(*c);
    rebuildHash();
    for (auto& [k, in] : interiors_) in->place(origin_->frame());
  }
  current_view_ = view_distance_m;  // affects rendering range; streaming radius is fixed per layer
  const rj::geo::Geodetic g = toGeodetic(player);
  rj::stream::Viewer v;
  v.pos = {g.lat_deg, g.lon_deg};
  v.altitude_m = std::max(0.0, player.z);
  streamer_->update(v);
  for (const auto& k : empty_completions_) streamer_->onLoaded(k, 0);
  empty_completions_.clear();

  // Collect finished jobs; upload at most one cell per frame to bound hitches.
  bool uploaded = false;
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    if (it->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++it;
      continue;
    }
    if (uploaded && !it->cancelled) {
      ++it;
      continue;
    }
    std::unique_ptr<CellCpu> cpu = it->fut.get();
    const rj::stream::StreamKey key = it->key;
    const bool cancelled = it->cancelled;
    it = jobs_.erase(it);
    if (cancelled) continue;
    if (!cpu) {
      streamer_->onLoaded(key, 0);  // unreadable: treat as empty so it is not retried every frame
      continue;
    }
    auto lc = std::make_unique<LoadedCell>();
    lc->meta = {cpu->mesh, cpu->anchor[0], cpu->anchor[1], cpu->anchor[2]};
    lc->frame = rj::geo::LocalFrame({cpu->anchor[0], cpu->anchor[1], cpu->anchor[2]});
    const size_t bytes = cpu->bytes;
    uploadCell(*cpu, lc->gpu);
    uploadDetail(cpu->detail, lc->gpu.detail);
    lc->cpu = std::move(cpu);
    placeCell(*lc);
    loaded_[lc->meta.mesh] = std::move(lc);
    rebuildHash();
    streamer_->onLoaded(key, bytes);
    uploaded = true;
  }
  if (interior_streamer_)
    interior_streamer_->update({g.lat_deg, g.lon_deg}, interior_candidates_,
                               [&](const rj::geo::MeshCode& m) { return loaded_.count(m.str()) > 0; });
  return rebased;
}

std::optional<double> World::terrainHeight(double x, double y) const {
  for (const auto& [code, c] : loaded_) {
    const int nx = c->cpu->tnx, ny = c->cpu->tny;
    if (nx < 2 || ny < 2) continue;
    const double vx = x - c->p00.x, vy = y - c->p00.y;
    const double det = c->ex.x * c->ey.y - c->ex.y * c->ey.x;
    if (std::abs(det) < 1e-9) continue;
    const double fj = (vx * c->ey.y - vy * c->ey.x) / det;
    const double fi = (c->ex.x * vy - c->ex.y * vx) / det;
    if (fi < 0 || fj < 0 || fi > ny - 1 || fj > nx - 1) continue;
    const int i = std::min(static_cast<int>(fi), ny - 2), j = std::min(static_cast<int>(fj), nx - 2);
    const double u = fi - i, w = fj - j;
    auto Z = [&](int a, int b) { return static_cast<double>(c->tz[static_cast<size_t>(a * nx + b)]); };
    // Exactly the rendered surface: quads are split along (i,j)-(i+1,j+1) (see prepareTerrain).
    const double a = Z(i, j), b = Z(i, j + 1), d = Z(i + 1, j), e = Z(i + 1, j + 1);
    return w >= u ? a + w * (b - a) + u * (e - b) : a + u * (d - a) + w * (e - d);
  }
  return std::nullopt;
}

namespace {
// Barycentric z of (x, y) inside triangle t (9 floats), or nullopt.
std::optional<double> triZ(const float* t, double x, double y) {
  const double d = (t[4] - t[7]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[1] - t[7]);
  if (std::fabs(d) < 1e-12) return std::nullopt;
  const double l1 = ((t[4] - t[7]) * (x - t[6]) + (t[6] - t[3]) * (y - t[7])) / d;
  const double l2 = ((t[7] - t[1]) * (x - t[6]) + (t[0] - t[6]) * (y - t[7])) / d;
  const double l3 = 1.0 - l1 - l2;
  if (l1 < -1e-5 || l2 < -1e-5 || l3 < -1e-5) return std::nullopt;
  return l1 * t[2] + l2 * t[5] + l3 * t[8];
}
}  // namespace

std::optional<double> World::surfaceHeight(double x, double y) const {
  const int64_t k = bucketKey(static_cast<int>(std::floor(x / 4.0)), static_cast<int>(std::floor(y / 4.0)));
  for (const auto& [code, c] : loaded_) {
    auto it = c->walk_hash.find(k);
    if (it == c->walk_hash.end()) continue;
    for (uint32_t t : it->second)
      if (auto z = triZ(&c->walk[static_cast<size_t>(t) * 9], x, y)) return z;
  }
  return roadHeight(x, y);
}

std::optional<double> World::roadHeight(double x, double y) const {
  const int64_t k = bucketKey(static_cast<int>(std::floor(x / 4.0)), static_cast<int>(std::floor(y / 4.0)));
  for (const auto& [code, c] : loaded_) {
    auto it = c->deck_hash.find(k);
    if (it == c->deck_hash.end()) continue;
    for (uint32_t t : it->second)
      if (auto z = triZ(&c->deck[static_cast<size_t>(t) * 9], x, y)) return z;
  }
  return terrainHeight(x, y);
}

std::optional<double> World::floorBelow(double x, double y, double zref) const {
  std::optional<double> best, above;  // highest surface not above zref / lowest one above it
  auto consider = [&](double z) {
    if (z <= zref) {
      if (!best || z > *best) best = z;
    } else if (!above || z < *above) {
      above = z;
    }
  };
  const int64_t k = bucketKey(static_cast<int>(std::floor(x / 4.0)), static_cast<int>(std::floor(y / 4.0)));
  for (const auto& [code, c] : loaded_) {
    if (auto it = c->walk_hash.find(k); it != c->walk_hash.end())
      for (uint32_t t : it->second)
        if (auto z = triZ(&c->walk[static_cast<size_t>(t) * 9], x, y)) consider(*z);
    if (auto it = c->deck_hash.find(k); it != c->deck_hash.end())
      for (uint32_t t : it->second)
        if (auto z = triZ(&c->deck[static_cast<size_t>(t) * 9], x, y)) consider(*z);
  }
  if (auto t = terrainHeight(x, y)) consider(*t);
  return best ? best : above;  // (below every surface, e.g. just after a teleport: climb onto the lowest)
}

void World::collideWalls(rj::geo::Vec3d& p, double radius, const std::vector<float>* extra) const {
  auto push = [&](const float* w) {
    const double feet = p.z;
    if (feet > w[5] - 0.3 || feet + 1.7 < w[4]) return;  // stepped over / passes under
    const double ax = w[0], ay = w[1], bx = w[2], by = w[3];
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 1e-9 ? ((p.x - ax) * dx + (p.y - ay) * dy) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double cx = ax + dx * t, cy = ay + dy * t;
    double nx = p.x - cx, ny = p.y - cy;
    const double d = std::hypot(nx, ny);
    if (d >= radius) return;
    if (d < 1e-6) {  // on the line: push along its normal
      const double l = std::sqrt(std::max(l2, 1e-9));
      nx = -dy / l;
      ny = dx / l;
    } else {
      nx /= d;
      ny /= d;
    }
    p.x = cx + nx * (radius + 0.001);
    p.y = cy + ny * (radius + 0.001);
  };
  for (int iter = 0; iter < 2; ++iter) {
    const int64_t k = bucketKey(static_cast<int>(std::floor(p.x / 4.0)), static_cast<int>(std::floor(p.y / 4.0)));
    for (const auto& [code, c] : loaded_) {
      auto it = c->wall_hash.find(k);
      if (it == c->wall_hash.end()) continue;
      for (uint32_t i : it->second) push(&c->walls[static_cast<size_t>(i) * 6]);
    }
    if (extra)
      for (size_t i = 0; i + 5 < extra->size(); i += 6) push(&(*extra)[i]);
  }
}

bool World::pointInBuilding(double x, double y) const {
  auto it = hash_.find(bucketKey(static_cast<int>(std::floor(x / kBucket)), static_cast<int>(std::floor(y / kBucket))));
  if (it == hash_.end()) return false;
  const Vector2 q{static_cast<float>(x), static_cast<float>(y)};
  for (const auto& [cell, b] : it->second) {
    const auto& poly = cell->fp[static_cast<size_t>(b)];
    if (poly.size() >= 3 && CheckCollisionPointPoly(q, poly.data(), static_cast<int>(poly.size()))) return true;
  }
  return false;
}

bool World::onCrosswalk(double x, double y) const {
  for (const auto& [code, c] : loaded_)
    for (size_t t = 0; t + 8 < c->cross.size(); t += 9)
      if (triZ(&c->cross[t], x, y)) return true;
  return false;
}

bool World::hasSurveyedMarking(double x, double y, double radius) const {
  const int r = static_cast<int>(std::ceil(radius / 2.0));
  const int bx = static_cast<int>(std::floor(x / 2.0)), by = static_cast<int>(std::floor(y / 2.0));
  for (const auto& [code, c] : loaded_) {
    if (c->mark_hash.empty()) continue;
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx)
        if (c->mark_hash.count(bucketKey(bx + dx, by + dy))) return true;
  }
  return false;
}

void World::collide(rj::geo::Vec3d& p, double radius) const {
  for (int iter = 0; iter < 3; ++iter) {
    const int bx = static_cast<int>(std::floor(p.x / kBucket)), by = static_cast<int>(std::floor(p.y / kBucket));
    bool moved = false;
    for (int dx = -1; dx <= 1 && !moved; ++dx)
      for (int dy = -1; dy <= 1 && !moved; ++dy) {
        auto it = hash_.find(bucketKey(bx + dx, by + dy));
        if (it == hash_.end()) continue;
        for (const auto& [cell, b] : it->second) {
          if (cell->cpu->buildings[static_cast<size_t>(b)].flags & kBuildingWalkIn) continue;  // stations: walls section instead
          if (p.z > cell->zmax[static_cast<size_t>(b)] + 0.3 || p.z + 1.8 < cell->zmin[static_cast<size_t>(b)]) continue;
          const auto& poly = cell->fp[static_cast<size_t>(b)];
          const Vector2 q{static_cast<float>(p.x), static_cast<float>(p.y)};
          float d;
          const Vector2 c = closestOnPoly(poly, q, d);
          const bool inside = pointInPoly(poly, q.x, q.y);
          if (!inside && d >= radius) continue;
          double nx = q.x - c.x, ny = q.y - c.y;
          double len = std::hypot(nx, ny);
          if (len < 1e-6) {
            nx = 1;
            ny = 0;
            len = 1;
          }
          nx /= len;
          ny /= len;
          if (inside) {
            nx = -nx;
            ny = -ny;
          }
          p.x = c.x + nx * (radius + 0.01);
          p.y = c.y + ny * (radius + 0.01);
          moved = true;
          break;
        }
      }
    if (!moved) break;
  }
}

std::optional<World::Hit> World::pick(const rj::geo::Vec3d& from, const rj::geo::Vec3d& dir, double max_dist) const {
  for (double t = 0.5; t < max_dist; t += 0.75) {
    const rj::geo::Vec3d p = from + dir * t;
    auto it = hash_.find(bucketKey(static_cast<int>(std::floor(p.x / kBucket)), static_cast<int>(std::floor(p.y / kBucket))));
    if (it == hash_.end()) continue;
    for (const auto& [cell, b] : it->second) {
      const auto bi = static_cast<size_t>(b);
      if (p.z < cell->zmin[bi] - 0.5 || p.z > cell->zmax[bi] + 0.5) continue;
      if (pointInPoly(cell->fp[bi], static_cast<float>(p.x), static_cast<float>(p.y)))
        return Hit{&cell->cpu->buildings[bi], cell, t};
    }
  }
  return std::nullopt;
}

rj::geo::Geodetic World::toGeodetic(const rj::geo::Vec3d& local) const {
  return rj::geo::ecefToGeodetic(origin_->frame().localToEcef(local));
}

rj::geo::Vec3d World::toLocal(const rj::geo::Geodetic& g) const { return origin_->frame().geodeticToLocal(g); }

bool World::insideData(double lat, double lon) const {
  const std::string code = rj::geo::MeshCode::fromLatLon({lat, lon}, 3) ? rj::geo::MeshCode::fromLatLon({lat, lon}, 3)->str() : "";
  return std::any_of(meta_.cells.begin(), meta_.cells.end(), [&](const CellMeta& m) { return m.mesh == code; });
}

std::optional<double> World::minTerrainZ() const {
  std::optional<double> m;
  for (const auto& [k, c] : loaded_)
    for (float z : c->tz) m = m ? std::min(*m, static_cast<double>(z)) : static_cast<double>(z);
  return m;
}

size_t World::buildingCount() const {
  size_t n = 0;
  for (const auto& [k, c] : loaded_) n += c->cpu->buildings.size();
  return n;
}

}  // namespace rjc
