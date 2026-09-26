#include <set>

#include "rj/geo/geoid.hpp"
#include "rj/geo/local_frame.hpp"
#include "rj/geo/mesh_code.hpp"
#include "rj/geo/plane_rect.hpp"
#include "rj_test.hpp"

using namespace rj::geo;

// Reference values generated with PROJ (pyproj 3.x), EPSG:6668 -> EPSG:6669..6687.
// Point = zone origin + (0.37° N, -0.52° E).
struct PlaneRef {
  int zone;
  double lat, lon, x, y;
};
static const PlaneRef kPlaneRefs[] = {
    {1, 33.37, 128.98, 41152.543215, -48387.250090},
    {2, 33.37, 130.48, 41152.543215, -48387.250090},
    {3, 36.37, 131.6466666667, 41177.558202, -46660.654564},
    {4, 33.37, 132.98, 41152.543215, -48387.250090},
    {5, 36.37, 133.8133333333, 41177.558202, -46660.654564},
    {6, 36.37, 135.48, 41177.558202, -46660.654564},
    {7, 36.37, 136.6466666667, 41177.558202, -46660.654564},
    {8, 36.37, 137.98, 41177.558202, -46660.654564},
    {9, 36.37, 139.3133333333, 41177.558202, -46660.654564},
    {10, 40.37, 140.3133333333, 41209.821656, -44159.892745},
    {11, 44.37, 139.73, 41240.266856, -41443.027490},
    {12, 44.37, 141.73, 41240.266856, -41443.027490},
    {13, 44.37, 143.73, 41240.266856, -41443.027490},
    {14, 26.37, 141.48, 41093.104198, -51892.287441},
    {15, 26.37, 126.98, 41093.104198, -51892.287441},
    {16, 26.37, 123.48, 41093.104198, -51892.287441},
    {17, 26.37, 130.48, 41093.104198, -51892.287441},
    {18, 20.37, 135.48, 41043.093679, -54283.350040},
    {19, 26.37, 153.48, 41093.104198, -51892.287441},
};

RJ_TEST(plane_rect_matches_proj_all_19_zones) {
  for (const auto& r : kPlaneRefs) {
    auto z = planeRectZone(r.zone);
    RJ_CHECK(z.has_value());
    const PlaneXY p = geodeticToPlane({r.lat, r.lon}, *z);
    RJ_CHECK_NEAR(p.x_north_m, r.x, 0.001);  // 1 mm
    RJ_CHECK_NEAR(p.y_east_m, r.y, 0.001);
    const LatLon back = planeToGeodetic(p, *z);
    RJ_CHECK_NEAR(back.lat_deg, r.lat, 1e-9);
    RJ_CHECK_NEAR(back.lon_deg, r.lon, 1e-9);
  }
}

RJ_TEST(plane_rect_shibuya_zone9) {
  // PROJ: JR Shibuya area (35.658, 139.7016) in zone IX.
  const PlaneXY p = geodeticToPlane({35.658, 139.7016}, *planeRectZone(9));
  RJ_CHECK_NEAR(p.x_north_m, -37935.107703, 0.001);
  RJ_CHECK_NEAR(p.y_east_m, -11927.445073, 0.001);
  RJ_CHECK(!planeRectZone(0).has_value());
  RJ_CHECK(!planeRectZone(20).has_value());
}

RJ_TEST(ecef_roundtrip) {
  const Geodetic g{35.658, 139.7016, 75.0};
  const Ecef e = geodeticToEcef(g);
  const Geodetic b = ecefToGeodetic(e);
  RJ_CHECK_NEAR(b.lat_deg, g.lat_deg, 1e-10);
  RJ_CHECK_NEAR(b.lon_deg, g.lon_deg, 1e-10);
  RJ_CHECK_NEAR(b.h_ellipsoidal_m, g.h_ellipsoidal_m, 1e-4);
  // Equator / prime meridian sanity.
  const Ecef q = geodeticToEcef({0, 0, 0});
  RJ_CHECK_NEAR(q.x, 6378137.0, 1e-6);
}

RJ_TEST(mesh_code_known_cells) {
  // PLATEAU file 53393585_tran_6697_op.gml has envelope lat 35.6497..35.6585, lon 139.6868..139.7003
  auto m = MeshCode::fromLatLon({35.655, 139.693}, 3);
  RJ_CHECK(m.has_value());
  RJ_CHECK_EQ(m->str(), std::string("53393585"));
  const GeoBBox b = m->bounds();
  RJ_CHECK_NEAR(b.min_lat, 35.65, 1e-12);
  RJ_CHECK_NEAR(b.max_lat, 35.658333333333, 1e-9);
  RJ_CHECK_NEAR(b.min_lon, 139.6875, 1e-12);
  RJ_CHECK_NEAR(b.max_lon, 139.7, 1e-12);
  // JR Shibuya station and Tokyo station.
  RJ_CHECK_EQ(MeshCode::fromLatLon({35.658, 139.7016}, 3)->str(), std::string("53393586"));
  RJ_CHECK_EQ(MeshCode::fromLatLon({35.6812, 139.7671}, 3)->str(), std::string("53394611"));
  RJ_CHECK_EQ(MeshCode::fromLatLon({35.6812, 139.7671}, 1)->str(), std::string("5339"));
  RJ_CHECK_EQ(MeshCode::fromLatLon({35.6812, 139.7671}, 2)->str(), std::string("533946"));
}

RJ_TEST(mesh_code_parse_roundtrip_all_levels) {
  const LatLon p{35.6595, 139.7005};
  for (int level = 1; level <= 6; ++level) {
    auto m = MeshCode::fromLatLon(p, level);
    RJ_CHECK(m.has_value());
    RJ_CHECK(m->bounds().contains(p));
    auto parsed = MeshCode::parse(m->str());
    RJ_CHECK(parsed.has_value());
    RJ_CHECK(*parsed == *m);
    RJ_CHECK_EQ(m->str().size(), static_cast<size_t>(level <= 3 ? 2 + 2 * level : 5 + level));
    if (level > 1) RJ_CHECK(m->parent(level - 1) == *MeshCode::fromLatLon(p, level - 1));
  }
  RJ_CHECK(!MeshCode::parse("5339358").has_value());  // bad length
  RJ_CHECK(!MeshCode::parse("53398586").has_value()); // 2nd-level digit 8 invalid
  RJ_CHECK(!MeshCode::parse("533935865").has_value()); // half-mesh digit must be 1..4
}

RJ_TEST(mesh_code_children_neighbors_cover) {
  auto m = *MeshCode::parse("53393586");
  const auto kids = m.children();
  RJ_CHECK_EQ(kids.size(), 4u);
  std::set<std::string> names;
  for (const auto& k : kids) names.insert(k.str());
  RJ_CHECK(names.count("533935861") && names.count("533935862") && names.count("533935863") &&
           names.count("533935864"));
  RJ_CHECK_EQ(m.neighbor(1, 0).str(), std::string("53393596"));
  RJ_CHECK_EQ(m.neighbor(0, -1).str(), std::string("53393585"));
  RJ_CHECK_EQ(m.neighbor(-1, 0).str(), std::string("53393576"));
  // Crossing a 2nd-level boundary: 53393599 east -> 53393690.
  RJ_CHECK_EQ(MeshCode::parse("53393599")->neighbor(0, 1).str(), std::string("53393690"));
  // The Shibuya 1 km vertical slice box covers exactly 4 third-level cells.
  const auto cells = MeshCode::cover({35.6535, 139.6961, 35.6625, 139.7071}, 3);
  RJ_CHECK_EQ(cells.size(), 4u);
  std::set<std::string> c;
  for (const auto& x : cells) c.insert(x.str());
  RJ_CHECK(c.count("53393585") && c.count("53393586") && c.count("53393595") && c.count("53393596"));
}

RJ_TEST(local_frame_and_floating_origin) {
  const LocalFrame f({35.658, 139.7016, 40.0});
  const Vec3d o = f.geodeticToLocal({35.658, 139.7016, 40.0});
  RJ_CHECK_NEAR(o.norm(), 0.0, 1e-6);
  // ~1 km north should be ~ +1000 m in y and slightly below the tangent plane.
  const Vec3d n = f.geodeticToLocal({35.658 + 1000.0 / 110950.0, 139.7016, 40.0});
  RJ_CHECK_NEAR(n.y, 1000.0, 3.0);
  RJ_CHECK(n.z < 0.0 && n.z > -0.2);  // curvature drop ~7.8 cm at 1 km

  // Transform between two cell frames is consistent with going via ECEF.
  const LocalFrame g({35.6625, 139.7071, 38.0});
  const Rigid3d T = f.transformFrom(g);
  const Vec3d p_in_g{12.0, -40.0, 5.0};
  const Vec3d via = f.ecefToLocal(g.localToEcef(p_in_g));
  const Vec3d direct = T.apply(p_in_g);
  RJ_CHECK_NEAR((via - direct).norm(), 0.0, 1e-6);

  FloatingOrigin fo({35.658, 139.7016, 40.0}, 1000.0);
  Vec3d player{500.0, 0.0, 0.0};
  RJ_CHECK(!fo.update(player));
  const Ecef before = fo.frame().localToEcef({1500.0, 200.0, 0.0});
  player = {1500.0, 200.0, 0.0};
  RJ_CHECK(fo.update(player));
  RJ_CHECK_EQ(fo.rebaseCount(), 1);
  RJ_CHECK(std::sqrt(player.x * player.x + player.y * player.y) < 1.0);
  const Ecef after = fo.frame().localToEcef(player);
  RJ_CHECK_NEAR(before.x, after.x, 1e-4);
  RJ_CHECK_NEAR(before.y, after.y, 1e-4);
  RJ_CHECK_NEAR(before.z, after.z, 1e-4);
}

RJ_TEST(geoid_grid_bilinear) {
  // Synthetic 2x2 grid (test fixture only, not real geoid data).
  GridGeoid g(35.0, 139.0, 1.0, 1.0, 2, 2, {36.0, 37.0, 38.0, 39.0}, "fixture");
  RJ_CHECK_NEAR(*g.undulation({35.5, 139.5}), 37.5, 1e-9);
  RJ_CHECK(!g.undulation({34.0, 139.5}).has_value());
  RJ_CHECK(!g.isApproximate());
  ConstantGeoid c(36.7);
  RJ_CHECK(c.isApproximate());
}
