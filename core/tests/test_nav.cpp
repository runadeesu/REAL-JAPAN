#include "rj/nav/grid_nav.hpp"
#include "rj_test.hpp"

using namespace rj::nav;

RJ_TEST(grid_nav_routes_around_buildings) {
  // 100 m x 60 m area, 1 m cells, a "building" wall with a gap near the top.
  GridNav g(100, 60, 1.0, 0.0, 0.0);
  g.blockPolygon({{45, 0}, {55, 0}, {55, 50}, {45, 50}});
  int cx, cy;
  RJ_CHECK(g.toCell({50, 20}, cx, cy) && g.blocked(cx, cy));
  RJ_CHECK(g.toCell({50, 55}, cx, cy) && !g.blocked(cx, cy));
  const auto p = g.findPath({10, 10}, {90, 10});
  RJ_CHECK(p.has_value());
  if (p) {
    for (size_t i = 1; i < p->size(); ++i) RJ_CHECK(g.lineOfSight((*p)[i - 1], (*p)[i]));
    // Must go over the wall through the gap: path longer than the straight 80 m.
    RJ_CHECK(GridNav::pathLength(*p) > 110.0);  // geometric optimum via the gap is ~116 m
    RJ_CHECK(p->size() <= 6);  // smoothed to a few turning points
    const Vec2 mid = GridNav::pointAt(*p, GridNav::pathLength(*p) / 2);
    RJ_CHECK(mid.y > 45.0);
  }
}

RJ_TEST(grid_nav_unreachable_and_snapping) {
  GridNav g(40, 40, 1.0, -20.0, -20.0);
  // A closed ring around the goal.
  g.blockPolygon({{5, 5}, {15, 5}, {15, 6}, {5, 6}});
  g.blockPolygon({{5, 14}, {15, 14}, {15, 15}, {5, 15}});
  g.blockPolygon({{5, 5}, {6, 5}, {6, 15}, {5, 15}});
  g.blockPolygon({{14, 5}, {15, 5}, {15, 15}, {14, 15}});
  RJ_CHECK(!g.findPath({-15, -15}, {10, 10}).has_value());
  // Starting inside a building footprint snaps to the nearest free cell.
  GridNav h(30, 30, 1.0, 0.0, 0.0);
  h.blockPolygon({{10, 10}, {20, 10}, {20, 20}, {10, 20}});
  const auto p = h.findPath({15, 15}, {2, 2});
  RJ_CHECK(p.has_value());
}

RJ_TEST(grid_nav_never_snaps_into_enclosed_pockets) {
  // Two buildings touching around a 2 m courtyard: the courtyard is free but enclosed.
  GridNav g(60, 60, 1.0, 0.0, 0.0);
  g.blockPolygon({{20, 20}, {40, 20}, {40, 29}, {20, 29}});
  g.blockPolygon({{20, 31}, {40, 31}, {40, 40}, {20, 40}});
  g.blockPolygon({{20, 29}, {29, 29}, {29, 31}, {20, 31}});
  g.blockPolygon({{31, 29}, {40, 29}, {40, 31}, {31, 31}});
  g.computeComponents();
  int cx, cy;
  RJ_CHECK(g.toCell({30, 30}, cx, cy) && !g.blocked(cx, cy) && !g.inMainComponent(cx, cy));
  const auto p = g.findPath({30, 30}, {5, 5});  // start inside the courtyard
  RJ_CHECK(p.has_value());
}
