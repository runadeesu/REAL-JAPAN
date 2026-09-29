#pragma once
// Single trees near the player in the fictional country's forests. Afar the forest is a canopy
// surface lifted a crown's height over the ground (world/canopy.cpp); within about 120 m of the
// camera the renderer cuts that surface away and these trees stand in its place: trunks and crowns
// (conifers, as the cedar and cypress plantations of Japanese hills, and rounded broadleaf trees
// in patches) planted on a jittered 5 m grid where the land cover is forest, as tall as the canopy
// there. Built per 40 m tile around the camera (merged meshes), rebuilt as it moves.

#include <cstdint>
#include <unordered_map>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;
class Renderer;

class NearTrees {
 public:
  static constexpr float kRadius = 125.0f;  // single trees within this distance
  static constexpr float kCut = 105.0f;     // the canopy gives way inside this radius
  void update(const World& world, const rj::geo::Vec3d& cam);
  void draw(Renderer& r) const;
  void clear();
  int treeCount() const { return trees_; }

 private:
  struct Tile {
    Mesh crowns{}, trunks{};
    int trees = 0;
  };
  std::unordered_map<int64_t, Tile> tiles_;
  rj::geo::Geodetic origin_{};
  bool have_origin_ = false;
  int trees_ = 0;
  Tile build(const World& world, int tx, int ty) const;
};

}  // namespace rjc
