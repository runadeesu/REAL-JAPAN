#pragma once
// Road markings and pedestrian crossings.
//
//  * SURVEYED: PLATEAU frn road markings (crosswalks 1110, lane lines, arrows) come with the street
//    detail (RJDET) and are drawn from there. Their crosswalk areas are grouped here into crossings
//    and tied to the nearest real traffic signal.
//  * ESTIMATED (not surveyed): PLATEAU covers markings on some roads only. Elsewhere markings are
//    derived from the real carriageway graph following Japanese practice (道路標示 / 区画線): zebra
//    crossings (45 cm bars, 45 cm gaps, 4 m wide) and stop lines on every arm of a junction with real
//    signal heads, centre lines on carriageways >= 5.5 m, lane lines and outside lines. Their exact
//    positions are inferred, not measured, and every generated crossing is flagged `estimated`.
//  * ESTIMATED signals: PLATEAU street furniture (frn 4900 heads) is surveyed on some roads only.
//    Major junctions (two or more arms >= 7.5 m wide) without surveyed heads get signals placed the
//    Japanese way (far-side vehicle heads over the departure lanes, pedestrian heads at each end of
//    each crossing), with generated poles and housings.

#include <cstddef>
#include <vector>

#include "raylib.h"
#include "rj/nav/grid_nav.hpp"

namespace rjc {

class World;
class Traffic;
class TrafficSignals;

struct Crossing {
  std::vector<std::vector<rj::nav::Vec2>> polys;  // area, origin ENU (x east, y north)
  rj::nav::Vec2 center;
  double heading = 0;         // walking direction across the road (compass radians, either sense)
  int group = -1, phase = 0;  // controlling signal (group -1: unsignalised)
  bool estimated = false;
};

class RoadMarkings {
 public:
  ~RoadMarkings();
  // Needs the traffic graph placed in the current origin; (re)assigns its signal groups.
  // Also places ESTIMATED signals (heads + hardware) at major junctions that have no surveyed heads.
  void build(Traffic& traffic, TrafficSignals& signals, const World& world);
  void unload();
  template <class F>
  void forEachMesh(F&& f) const {
    for (const auto& m : meshes_) f(m);
  }
  const std::vector<Crossing>& crossings() const { return crossings_; }
  size_t estimatedCrossings() const { return n_est_crossings_; }
  size_t estimatedSignalHeads() const { return n_est_signals_; }
  size_t triangles() const { return tris_; }

 private:
  std::vector<Mesh> meshes_;
  std::vector<Crossing> crossings_;
  size_t n_est_crossings_ = 0;
  size_t n_est_signals_ = 0;
  size_t tris_ = 0;
};

}  // namespace rjc
