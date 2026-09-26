#pragma once
// Verified building interiors (RJINT, see pipeline/realjapan_pipeline/interior.py).
// Geometry and walkable/blocking surfaces come from PLATEAU LOD4; nothing is invented.

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

struct InteriorMeta {
  std::string id, file, name, status;
  int source_index = 0;
  std::vector<rj::geo::Geodetic> entrances;  // street-level openings
  std::vector<std::vector<rj::geo::Geodetic>> openings;  // stairwell outlines (h = 0), for pedestrian navigation
};

class Interior {
 public:
  bool load(const std::filesystem::path& file, std::string& err);  // CPU parse + GPU upload
  void unload();
  void place(const rj::geo::LocalFrame& origin);  // (re)compute origin-space data

  const std::string& id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::vector<Mesh>& meshes() const { return meshes_; }
  const Matrix& model() const { return model_; }

  struct Entrance {
    rj::geo::Vec3d street, inside;  // origin ENU
  };
  const std::vector<Entrance>& entrances() const { return ents_; }

  // Highest walkable surface at (x, y) not above z_top and not more than max_drop below it.
  std::optional<double> floorBelow(double x, double y, double z_top, double max_drop) const;
  void collide(rj::geo::Vec3d& p, double radius) const;
  // Distance along the segment to the first wall/floor triangle hit (for the third-person camera).
  std::optional<double> raycast(const rj::geo::Vec3d& from, const rj::geo::Vec3d& dir_unit, double max_d) const;
  bool contains(double x, double y) const;
  // Street-level openings (stairwell lids, PLATEAU ClosureSurface): where the pavement opens into this space.
  bool overOpening(double x, double y) const;
  double distanceToOpening(double x, double y) const;  // to the nearest opening triangle centroid

 private:
  struct Tri {
    float a[3], b[3], c[3];
  };
  void rebuildHash();

  std::string id_, name_;
  rj::geo::LocalFrame frame_{rj::geo::Geodetic{}};
  std::vector<Mesh> meshes_;
  Matrix model_{};
  std::vector<Tri> floors_local_, walls_local_, openings_local_;  // interior ENU
  std::vector<Tri> floors_, walls_, openings_;                    // origin ENU
  std::vector<std::pair<rj::geo::Vec3d, rj::geo::Vec3d>> ents_local_;
  std::vector<Entrance> ents_;
  float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
  static constexpr double kBucket = 4.0;
  std::unordered_map<int64_t, std::vector<uint32_t>> floor_hash_, wall_hash_;
};

}  // namespace rjc
