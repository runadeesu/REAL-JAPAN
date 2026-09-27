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

RJ_TEST(grid_nav_keeps_to_sidewalks_and_crossings) {
  // A 20 m wide carriageway (x 40..60) with a 4 m crossing at y 78..82; sidewalks on both sides.
  GridNav g(100, 100, 1.0, 0.0, 0.0);
  g.costPolygon({{40, 0}, {60, 0}, {60, 100}, {40, 100}}, 60);
  g.costPolygon({{40, 78}, {60, 78}, {60, 82}, {40, 82}}, GridNav::kBaseCost);
  int cx, cy;
  RJ_CHECK(g.toCell({50, 20}, cx, cy) && g.cost(cx, cy) == 60);
  RJ_CHECK(g.toCell({50, 80}, cx, cy) && g.cost(cx, cy) == GridNav::kBaseCost);
  const auto p = g.findPath({20, 20}, {80, 20});
  RJ_CHECK(p.has_value());
  if (p) {
    // Walks up to the crossing and back rather than straight across (60 m straight, ~140 m via the crossing).
    RJ_CHECK(GridNav::pathLength(*p) > 120.0);
    for (double d = 0; d < GridNav::pathLength(*p); d += 0.5) {
      const Vec2 q = GridNav::pointAt(*p, d);
      if (q.x > 41 && q.x < 59) RJ_CHECK(q.y > 76.5 && q.y < 83.5);  // on the carriageway only at the crossing
    }
  }
  // A narrow shared street (cost 12) is still crossed directly.
  GridNav h(100, 100, 1.0, 0.0, 0.0);
  h.costSegment({50, 0}, {50, 100}, 2.0, 12);
  const auto q = h.findPath({20, 20}, {80, 20});
  RJ_CHECK(q.has_value() && GridNav::pathLength(*q) < 62.0);
  // A wide road blocked mid-block except at a junction (y 90..100): the route goes round by it.
  GridNav k(100, 100, 1.0, 0.0, 0.0);
  k.blockSegment({50, 0}, {50, 88}, 9.0);
  RJ_CHECK(k.toCell({50, 40}, cx, cy) && k.blocked(cx, cy));
  RJ_CHECK(k.toCell({30, 40}, cx, cy) && !k.blocked(cx, cy));
  const auto r = k.findPath({20, 20}, {80, 20});
  RJ_CHECK(r.has_value() && GridNav::pathLength(*r) > 150.0);
  k.blockSegment({40, 20}, {60, 20}, 1.5, false);  // a mid-block crossing frees a passage
  const auto r2 = k.findPath({20, 20}, {80, 20});
  RJ_CHECK(r2.has_value() && GridNav::pathLength(*r2) < 70.0);
}
