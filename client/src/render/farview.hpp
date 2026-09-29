#pragma once
// Far view of the fictional country (data/world/country/far.*): the whole country's terrain on a
// coarse lat/lon grid (about 40 x 50 m) with a colour map, and boxes for the buildings that read
// from afar, split into tiles matching the world's cells. Tiles whose cell is loaded are not drawn
// (the detailed cell is there), so mountains, coasts and town skylines stay visible to the
// horizon while only the cells near the player are streamed. Also holds the snow-potential map
// the lit shader samples (see Renderer::setSnowMap).

#include <filesystem>
#include <string>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

class FarView {
 public:
  bool load(const std::filesystem::path& dir);  // CPU data (call before build)
  void build();                                 // GPU meshes / textures (window must exist)
  void unload();
  bool ready() const { return built_; }

  struct Tile {
    std::string mesh;                      // JIS 3rd-mesh code of the cell it stands in for
    rj::geo::LocalFrame frame{rj::geo::Geodetic{}};  // tile centre at sea level
    Mesh terrain{};
    // (stitching to loaded neighbours) the vertex positions as built, the grid size, the top vertex
    // each skirt vertex hangs from, and whether the mesh is currently stitched
    std::vector<float> pos0;
    int n = 0;
    std::vector<int> skirt_src;
    bool stitched = false;
    std::vector<Mesh> boxes;               // building boxes (split to stay within 16-bit indices)
    float radius = 0;                      // bounding radius about the centre (m)
    float zmax = 0;                        // highest ground in the tile (m above sea)
  };
  const std::vector<Tile>& tiles() const { return tiles_; }
  Texture2D colorMap() const { return color_; }
  Texture2D snowMap() const { return snow_; }
  // snow potential (0..1: where the winter's snow lies) at a place; -1 outside the map
  float snowPotential(double lat, double lon) const;
  // extent of the colour map (degrees): south, west, north, east
  void extent(double& lat0, double& lon0, double& lat1, double& lon1) const {
    lat0 = lat0_;
    lon0 = lon0_;
    lat1 = lat1_;
    lon1 = lon1_;
  }
  // raylib (x, z) -> snow / colour map uv for the world's current floating origin
  void mapping(const World& world, Vector3& u, Vector3& v) const;
  // Where a tile borders loaded cells, move its edge onto their ground (and canopy) so the two meet
  // without a wall or a gap; restore it when they unload. Cheap when nothing changed.
  void stitch(const World& world);

 private:
  bool loaded_ = false, built_ = false;
  double lat0_ = 0, lon0_ = 0, lat1_ = 0, lon1_ = 0;
  int nx_ = 0, ny_ = 0;                    // height grid (north row first)
  std::vector<uint16_t> height_;
  double frame_lat_ = 0, frame_lon_ = 0;   // country frame of the building boxes
  struct Box {
    float x, y, hl, hw, yaw, ground, height;
  };
  std::vector<Box> boxes_;
  Image color_img_{}, snow_img_{};
  std::vector<unsigned char> snow_cpu_;
  int snow_w_ = 0, snow_h_ = 0;
  Texture2D color_{}, snow_{};
  std::vector<Tile> tiles_;
  std::string stitch_sig_;
  float heightAt(int row, int col) const;  // metres above sea
};

}  // namespace rjc
