#pragma once
// Procedural Building Detail System (near field).
//
// PLATEAU gives the real footprint, height, storey count and usage of every building. This
// system adds the eye-level detail the survey data does not contain: windows with frames and
// sills, glazed shop fronts with mullions and doors, sign bands (blank: shop names are not in
// the data and are never invented), awnings, lobby glazing, house entrances with canopies,
// apartment balconies with parapets, AC outdoor units and drain pipes.
//
// Honesty: the positions of these details are ESTIMATED (procedural, seeded per building from
// its id, usage, storeys and street frontage). They are labelled as estimated in the developer
// overlay and in docs/STATUS.md. Photo-textured facades keep their real upper-floor imagery:
// no invented windows are painted over them.
//
// LOD: full detail < 60 m, lite (glass, slabs, sign bands) < 160 m, nothing beyond (PLATEAU
// geometry only). Generated incrementally with a per-frame budget.

#include <string>
#include <unordered_map>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

class FacadeDetail {
 public:
  void update(const World& world, const rj::geo::Vec3d& player, int budget_per_frame);
  void clear();  // origin rebase / world reload
  template <class F>
  void forEachMesh(F f) const {
    for (const auto& [k, e] : map_)
      for (const auto& m : e.meshes) f(m, e.lod);
  }
  // Night: shop-front light positions near the camera (origin ENU).
  void collectLights(const rj::geo::Vec3d& cam, double max_d, std::vector<rj::geo::Vec3d>& out) const;
  size_t buildingCount() const { return map_.size(); }
  size_t vertexCount() const { return verts_; }

 private:
  struct Entry {
    std::vector<Mesh> meshes;
    std::vector<rj::geo::Vec3d> lights;
    int lod = -1;
    bool alive = false;
  };
  void release(Entry& e);
  std::unordered_map<std::string, Entry> map_;
  size_t verts_ = 0;
};

}  // namespace rjc
