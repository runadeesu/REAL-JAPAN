#pragma once
// Japan Plane Rectangular Coordinate System (平面直角座標系), zones I..XIX,
// JGD2011 (EPSG:6669..6687). Gauss-Krueger projection using the GSI
// high-order series (Kawase 2011, 国土地理院時報 121), scale factor 0.9999.
//
// Axis convention follows the Japanese survey standard: X = northing,
// Y = easting (metres from the zone origin).

#include <optional>

#include "rj/geo/ellipsoid.hpp"

namespace rj::geo {

struct PlaneXY {
  double x_north_m = 0.0;
  double y_east_m = 0.0;
};

struct PlaneRectZone {
  int number;          // 1..19
  double origin_lat;   // degrees
  double origin_lon;   // degrees
  int epsg_jgd2011;    // 6668 + number
};

// Returns std::nullopt for an invalid zone number.
std::optional<PlaneRectZone> planeRectZone(int number);

PlaneXY geodeticToPlane(const LatLon& g, const PlaneRectZone& zone);
LatLon planeToGeodetic(const PlaneXY& p, const PlaneRectZone& zone);

}  // namespace rj::geo
