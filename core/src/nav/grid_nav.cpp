#include "rj/nav/grid_nav.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace rj::nav {

GridNav::GridNav(int width, int height, double cell_m, double origin_x, double origin_y)
    : w_(width), h_(height), cell_(cell_m), ox_(origin_x), oy_(origin_y),
      blocked_(static_cast<size_t>(width) * static_cast<size_t>(height), 0) {}

void GridNav::setBlocked(int cx, int cy, bool b) {
  if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return;
  blocked_[static_cast<size_t>(cy) * w_ + cx] = b ? 1 : 0;
}

bool GridNav::blocked(int cx, int cy) const {
  if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return true;
  return blocked_[static_cast<size_t>(cy) * w_ + cx] != 0;
}

bool GridNav::toCell(const Vec2& p, int& cx, int& cy) const {
  cx = static_cast<int>(std::floor((p.x - ox_) / cell_));
  cy = static_cast<int>(std::floor((p.y - oy_) / cell_));
  return cx >= 0 && cy >= 0 && cx < w_ && cy < h_;
}

Vec2 GridNav::cellCenter(int cx, int cy) const { return {ox_ + (cx + 0.5) * cell_, oy_ + (cy + 0.5) * cell_}; }

template <class F>
void GridNav::scanPolygon(const std::vector<Vec2>& poly, F&& f) const {
  if (poly.size() < 3) return;
  double y0 = 1e300, y1 = -1e300;
  for (const auto& p : poly) {
    y0 = std::min(y0, p.y);
    y1 = std::max(y1, p.y);
  }
  const int cy0 = std::max(0, static_cast<int>(std::floor((y0 - oy_) / cell_)));
  const int cy1 = std::min(h_ - 1, static_cast<int>(std::floor((y1 - oy_) / cell_)));
  std::vector<double> xs;
  for (int cy = cy0; cy <= cy1; ++cy) {
    const double y = oy_ + (cy + 0.5) * cell_;
    xs.clear();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
      const Vec2 a = poly[i], b = poly[j];
      if ((a.y > y) != (b.y > y)) xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
    }
    std::sort(xs.begin(), xs.end());
    for (size_t k = 0; k + 1 < xs.size(); k += 2) {
      const int cx0 = std::max(0, static_cast<int>(std::ceil((xs[k] - ox_) / cell_ - 0.5)));
      const int cx1 = std::min(w_ - 1, static_cast<int>(std::floor((xs[k + 1] - ox_) / cell_ - 0.5)));
      for (int cx = cx0; cx <= cx1; ++cx) f(static_cast<size_t>(cy) * w_ + cx);
    }
  }
}

void GridNav::blockPolygon(const std::vector<Vec2>& poly) {
  scanPolygon(poly, [&](size_t i) { blocked_[i] = 1; });
}

std::vector<size_t> GridNav::cellsInPolygon(const std::vector<Vec2>& poly) const {
  std::vector<size_t> out;
  scanPolygon(poly, [&](size_t i) { out.push_back(i); });
  return out;
}

void GridNav::setCost(int cx, int cy, uint8_t c) {
  if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return;
  if (cost_.empty()) cost_.assign(blocked_.size(), kBaseCost);
  cost_[static_cast<size_t>(cy) * w_ + cx] = std::max(c, kBaseCost);
}

uint8_t GridNav::cost(int cx, int cy) const {
  if (cost_.empty() || cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return kBaseCost;
  return cost_[static_cast<size_t>(cy) * w_ + cx];
}

void GridNav::costPolygon(const std::vector<Vec2>& poly, uint8_t c) {
  if (cost_.empty()) cost_.assign(blocked_.size(), kBaseCost);
  const uint8_t v = std::max(c, kBaseCost);
  scanPolygon(poly, [&](size_t i) { cost_[i] = v; });
}

void GridNav::costSegment(const Vec2& a, const Vec2& b, double radius, uint8_t c) {
  if (cost_.empty()) cost_.assign(blocked_.size(), kBaseCost);
  const uint8_t v = std::max(c, kBaseCost);
  const int x0 = std::max(0, static_cast<int>(std::floor((std::min(a.x, b.x) - radius - ox_) / cell_)));
  const int x1 = std::min(w_ - 1, static_cast<int>(std::floor((std::max(a.x, b.x) + radius - ox_) / cell_)));
  const int y0 = std::max(0, static_cast<int>(std::floor((std::min(a.y, b.y) - radius - oy_) / cell_)));
  const int y1 = std::min(h_ - 1, static_cast<int>(std::floor((std::max(a.y, b.y) + radius - oy_) / cell_)));
  const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
  for (int cy = y0; cy <= y1; ++cy)
    for (int cx = x0; cx <= x1; ++cx) {
      const Vec2 p = cellCenter(cx, cy);
      const double t = l2 > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
      if (std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy)) <= radius) cost_[static_cast<size_t>(cy) * w_ + cx] = v;
    }
}

void GridNav::blockSegment(const Vec2& a, const Vec2& b, double radius, bool block) {
  const int x0 = std::max(0, static_cast<int>(std::floor((std::min(a.x, b.x) - radius - ox_) / cell_)));
  const int x1 = std::min(w_ - 1, static_cast<int>(std::floor((std::max(a.x, b.x) + radius - ox_) / cell_)));
  const int y0 = std::max(0, static_cast<int>(std::floor((std::min(a.y, b.y) - radius - oy_) / cell_)));
  const int y1 = std::min(h_ - 1, static_cast<int>(std::floor((std::max(a.y, b.y) + radius - oy_) / cell_)));
  const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
  for (int cy = y0; cy <= y1; ++cy)
    for (int cx = x0; cx <= x1; ++cx) {
      const Vec2 p = cellCenter(cx, cy);
      const double t = l2 > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
      if (std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy)) <= radius) blocked_[static_cast<size_t>(cy) * w_ + cx] = block ? 1 : 0;
    }
}

void GridNav::computeComponents() {
  const size_t n = static_cast<size_t>(w_) * static_cast<size_t>(h_);
  comp_.assign(n, -1);
  std::vector<int32_t> stack;
  std::vector<size_t> sizes;
  for (size_t start = 0; start < n; ++start) {
    if (blocked_[start] || comp_[start] >= 0) continue;
    const int32_t label = static_cast<int32_t>(sizes.size());
    size_t count = 0;
    stack.push_back(static_cast<int32_t>(start));
    comp_[start] = label;
    while (!stack.empty()) {
      const int32_t c = stack.back();
      stack.pop_back();
      ++count;
      const int x = c % w_, y = c / w_;
      const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
      for (const auto& q : nb) {
        if (q[0] < 0 || q[1] < 0 || q[0] >= w_ || q[1] >= h_) continue;
        const size_t i = static_cast<size_t>(q[1]) * w_ + q[0];
        if (blocked_[i] || comp_[i] >= 0) continue;
        comp_[i] = label;
        stack.push_back(static_cast<int32_t>(i));
      }
    }
    sizes.push_back(count);
  }
  main_comp_ = sizes.empty() ? -1 : static_cast<int32_t>(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
}

bool GridNav::inMainComponent(int cx, int cy) const {
  if (blocked(cx, cy)) return false;
  if (comp_.empty()) return true;
  return comp_[static_cast<size_t>(cy) * w_ + cx] == main_comp_;
}

std::optional<Vec2> GridNav::nearestFree(const Vec2& p, int max_r) const {
  int cx, cy;
  toCell(p, cx, cy);
  if (inMainComponent(cx, cy)) return cellCenter(cx, cy);
  for (int r = 1; r <= max_r; ++r)
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        if (inMainComponent(cx + dx, cy + dy)) return cellCenter(cx + dx, cy + dy);
      }
  return std::nullopt;
}

bool GridNav::lineOfSight(const Vec2& a, const Vec2& b, uint8_t max_cost) const {
  const double len = std::hypot(b.x - a.x, b.y - a.y);
  const int steps = std::max(1, static_cast<int>(std::ceil(len / (cell_ * 0.4))));
  for (int i = 0; i <= steps; ++i) {
    const double t = static_cast<double>(i) / steps;
    int cx, cy;
    if (!toCell({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, cx, cy) || blocked(cx, cy)) return false;
    if (cost(cx, cy) > max_cost) return false;
  }
  return true;
}

std::optional<std::vector<Vec2>> GridNav::findPath(const Vec2& from, const Vec2& to, size_t max_exp) const {
  // Large footprints (stations, campuses): search up to ~160 m for a walkable cell.
  const auto s = nearestFree(from, 80), g = nearestFree(to, 80);
  if (!s || !g) return std::nullopt;
  int sx, sy, gx, gy;
  toCell(*s, sx, sy);
  toCell(*g, gx, gy);
  const size_t n = static_cast<size_t>(w_) * static_cast<size_t>(h_);
  std::vector<float> gcost(n, 1e30f);
  std::vector<int32_t> parent(n, -1);
  std::vector<uint8_t> closed(n, 0);
  auto idx = [&](int x, int y) { return static_cast<size_t>(y) * w_ + x; };
  auto hfun = [&](int x, int y) {
    const float dx = static_cast<float>(std::abs(x - gx)), dy = static_cast<float>(std::abs(y - gy));
    return (dx + dy) + (1.41421356f - 2.0f) * std::min(dx, dy);
  };
  using Node = std::pair<float, int32_t>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
  gcost[idx(sx, sy)] = 0;
  open.push({hfun(sx, sy), static_cast<int32_t>(idx(sx, sy))});
  size_t expansions = 0;
  bool found = false;
  static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static const int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!open.empty() && expansions < max_exp) {
    const auto [f, cur] = open.top();
    open.pop();
    if (closed[static_cast<size_t>(cur)]) continue;
    closed[static_cast<size_t>(cur)] = 1;
    ++expansions;
    const int cx = cur % w_, cy = cur / w_;
    if (cx == gx && cy == gy) {
      found = true;
      break;
    }
    for (int k = 0; k < 8; ++k) {
      const int nx = cx + DX[k], ny = cy + DY[k];
      if (blocked(nx, ny)) continue;
      if (k >= 4 && (blocked(cx + DX[k], cy) || blocked(cx, cy + DY[k]))) continue;  // no corner cutting
      const size_t ni = idx(nx, ny);
      const float step = (k >= 4 ? 1.41421356f : 1.0f) * static_cast<float>(cost(cx, cy) + cost(nx, ny)) / (2.0f * kBaseCost);
      const float ng = gcost[static_cast<size_t>(cur)] + step;
      if (ng < gcost[ni]) {
        gcost[ni] = ng;
        parent[ni] = cur;
        open.push({ng + hfun(nx, ny), static_cast<int32_t>(ni)});
      }
    }
  }
  if (!found) return std::nullopt;
  std::vector<Vec2> raw;
  std::vector<uint8_t> rawc;
  for (int32_t c = static_cast<int32_t>(idx(gx, gy)); c != -1; c = parent[static_cast<size_t>(c)]) {
    raw.push_back(cellCenter(c % w_, c / w_));
    rawc.push_back(cost(c % w_, c / w_));
  }
  std::reverse(raw.begin(), raw.end());
  std::reverse(rawc.begin(), rawc.end());
  // String pulling: keep only turning points that are needed for line of sight, and never
  // shortcut through cells dearer than the ones the search chose (sidewalk -> crossing).
  std::vector<Vec2> out{raw.front()};
  size_t anchor = 0;
  uint8_t cap = rawc.front();
  for (size_t i = 1; i < raw.size(); ++i) {
    const uint8_t capi = std::max(cap, rawc[i]);
    if (i >= 2 && !lineOfSight(raw[anchor], raw[i], capi)) {
      out.push_back(raw[i - 1]);
      anchor = i - 1;
      cap = std::max(rawc[i - 1], rawc[i]);
    } else {
      cap = capi;
    }
  }
  if (raw.size() > 1) out.push_back(raw.back());
  return out;
}

double GridNav::pathLength(const std::vector<Vec2>& p) {
  double L = 0;
  for (size_t i = 1; i < p.size(); ++i) L += std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
  return L;
}

Vec2 GridNav::pointAt(const std::vector<Vec2>& p, double d, Vec2* dir) {
  if (p.empty()) return {};
  if (p.size() == 1 || d <= 0) {
    if (dir && p.size() > 1) {
      const double l = std::hypot(p[1].x - p[0].x, p[1].y - p[0].y);
      *dir = l > 0 ? Vec2{(p[1].x - p[0].x) / l, (p[1].y - p[0].y) / l} : Vec2{0, 1};
    }
    return p.front();
  }
  for (size_t i = 1; i < p.size(); ++i) {
    const double l = std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    if (d <= l || i + 1 == p.size()) {
      const double t = l > 0 ? std::min(1.0, d / l) : 1.0;
      if (dir) *dir = l > 0 ? Vec2{(p[i].x - p[i - 1].x) / l, (p[i].y - p[i - 1].y) / l} : Vec2{0, 1};
      return {p[i - 1].x + (p[i].x - p[i - 1].x) * t, p[i - 1].y + (p[i].y - p[i - 1].y) * t};
    }
    d -= l;
  }
  return p.back();
}

}  // namespace rj::nav
