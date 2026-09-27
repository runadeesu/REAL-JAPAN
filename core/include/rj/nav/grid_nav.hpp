#pragma once
// Pedestrian navigation on a walkability grid (A*, 8-connected, no corner
// cutting) with line-of-sight path smoothing. The grid is built by the
// client from real building footprints (PLATEAU): walkable = inside data
// coverage and not inside a building. Cells carry a traversal cost (in tenths,
// 10 = 1.0) so routes keep to sidewalks and use crossings instead of cutting
// across carriageways; smoothing never shortcuts through dearer cells.

#include <cstdint>
#include <optional>
#include <vector>

namespace rj::nav {

struct Vec2 {
  double x = 0, y = 0;  // metres, x = east, y = north (origin-ENU)
};

class GridNav {
 public:
  GridNav() = default;
  GridNav(int width, int height, double cell_m, double origin_x, double origin_y);

  int width() const { return w_; }
  int height() const { return h_; }
  double cellSize() const { return cell_; }
  bool valid() const { return w_ > 0 && h_ > 0; }

  void setBlocked(int cx, int cy, bool b);
  bool blocked(int cx, int cy) const;
  bool toCell(const Vec2& p, int& cx, int& cy) const;
  Vec2 cellCenter(int cx, int cy) const;
  // Fill a polygon (world coordinates) as blocked.
  void blockPolygon(const std::vector<Vec2>& poly);

  // Traversal cost in tenths (10 = 1.0, the minimum; 255 max). Free cells default to 10.
  static constexpr uint8_t kBaseCost = 10;
  void setCost(int cx, int cy, uint8_t c);
  uint8_t cost(int cx, int cy) const;
  void costPolygon(const std::vector<Vec2>& poly, uint8_t c);
  // Linear indices (y * width + x) of the cells whose centre lies inside the polygon.
  std::vector<size_t> cellsInPolygon(const std::vector<Vec2>& poly) const;
  // Cells whose centre lies within `radius` of segment ab.
  void costSegment(const Vec2& a, const Vec2& b, double radius, uint8_t c);

  // Label 4-connected free regions; the largest one (the street network) becomes
  // the only valid snap target, so routes never start in an enclosed pocket
  // between adjacent buildings. Call after all obstacles are set.
  void computeComponents();
  bool inMainComponent(int cx, int cy) const;

  std::optional<Vec2> nearestFree(const Vec2& p, int max_radius_cells) const;
  // Straight walk from a to b stays on free cells whose cost is <= max_cost.
  bool lineOfSight(const Vec2& a, const Vec2& b, uint8_t max_cost = 255) const;
  // Path from `from` to `to` (both snapped to the nearest free cell).
  std::optional<std::vector<Vec2>> findPath(const Vec2& from, const Vec2& to, size_t max_expansions = 400000) const;

  static double pathLength(const std::vector<Vec2>& path);
  // Point at distance `d` along the path (clamped).
  static Vec2 pointAt(const std::vector<Vec2>& path, double d, Vec2* dir_out = nullptr);

 private:
  int w_ = 0, h_ = 0;
  double cell_ = 1.0, ox_ = 0.0, oy_ = 0.0;
  template <class F>
  void scanPolygon(const std::vector<Vec2>& poly, F&& f) const;
  std::vector<uint8_t> blocked_;
  std::vector<uint8_t> cost_;  // empty = every free cell at kBaseCost
  std::vector<int32_t> comp_;  // component label per cell (-1 blocked), empty = not computed
  int32_t main_comp_ = -1;
};

}  // namespace rj::nav
