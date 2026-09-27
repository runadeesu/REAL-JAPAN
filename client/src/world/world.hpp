#pragma once
// The streamed world of one slice: metadata, cell streaming through
// rj::stream::HierarchicalStreamer, the floating origin, and spatial queries
// (terrain height, building collision, picking) in origin-ENU coordinates.

#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"
#include "rj/stream/streamer.hpp"
#include "world/cell.hpp"
#include "world/interior.hpp"

namespace rjc {

struct CellMeta {
  std::string mesh;
  double lat = 0, lon = 0, h = 0;
};
struct Poi {
  double lat = 0, lon = 0;
  int usage = 0;
  float height = 0;
  std::string name;
};
struct SourceMeta {
  std::string license, attribution;
};
struct SliceMeta {
  std::string name_ja, name_en;
  double spawn_lat = 0, spawn_lon = 0, spawn_heading = 0;
  double core_bbox[4] = {0, 0, 0, 0};
  bool fictional = false;  // "world fictional": an invented island (sea all around, no source data)
  std::vector<CellMeta> cells;
  std::vector<Poi> pois;
  std::vector<SourceMeta> sources;
  std::vector<InteriorMeta> interiors;
};

struct LoadedCell {
  CellMeta meta;
  std::unique_ptr<CellCpu> cpu;
  CellGpu gpu;
  rj::geo::LocalFrame frame{rj::geo::Geodetic{}};
  rj::geo::Rigid3d to_origin;
  Matrix model{};
  // Origin-ENU data for queries.
  std::vector<std::vector<Vector2>> fp;  // per-building footprint (x=east, y=north)
  std::vector<float> zmin, zmax;
  std::vector<float> tx, ty, tz;  // terrain grid
  Vector2 ex{}, ey{}, p00{};      // affine approximation of the terrain grid
  // Street detail in origin ENU.
  std::vector<float> walk;                 // raised walkable triangles (9 floats each)
  std::unordered_map<int64_t, std::vector<uint32_t>> walk_hash;  // 4 m buckets -> triangle index
  std::vector<float> cross;                // crosswalk triangles
  std::vector<float> deck;                 // bridge decks (drivable, 9 floats per triangle)
  std::unordered_map<int64_t, std::vector<uint32_t>> deck_hash;
  std::unordered_set<int64_t> mark_hash;   // 2 m buckets holding surveyed road markings
  struct Light {
    rj::geo::Vec3d pos;
    float range;
  };
  std::vector<Light> lights;
  struct Signal {
    rj::geo::Vec3d pos;
    float axis_yaw, facing_yaw, length;
    int kind, group, phase;
  };
  std::vector<Signal> signals;
};

class World : public rj::stream::ICellIO, public rj::stream::IInteriorIO {
 public:
  World();
  ~World() override;

  bool loadMeta(const std::filesystem::path& slice_dir, std::string& err);
  const SliceMeta& meta() const { return meta_; }

  void resetOrigin(const rj::geo::Geodetic& g);
  const rj::geo::FloatingOrigin& origin() const { return *origin_; }

  // Per-frame streaming. May rebase the floating origin (player_local is
  // rewritten into the new frame, return value true).
  bool update(rj::geo::Vec3d& player_local, double view_distance_m);
  void unloadAll();

  std::optional<double> terrainHeight(double x, double y) const;
  // Walkable surface: raised sidewalks / traffic islands (real PLATEAU areas) where present, else terrain.
  std::optional<double> surfaceHeight(double x, double y) const;
  bool onCrosswalk(double x, double y) const;
  // Drivable surface: bridge decks where present, else terrain.
  std::optional<double> roadHeight(double x, double y) const;
  // Surveyed PLATEAU road markings (frn 1000-1299) within ~radius of (x, y).
  bool hasSurveyedMarking(double x, double y, double radius) const;
  bool pointInBuilding(double x, double y) const;  // inside any PLATEAU footprint
  void collide(rj::geo::Vec3d& p, double radius) const;
  struct Hit {
    const BuildingInfo* building = nullptr;
    const LoadedCell* cell = nullptr;
    double distance = 0;
  };
  std::optional<Hit> pick(const rj::geo::Vec3d& from, const rj::geo::Vec3d& dir, double max_dist) const;

  rj::geo::Geodetic toGeodetic(const rj::geo::Vec3d& local) const;
  rj::geo::Vec3d toLocal(const rj::geo::Geodetic& g) const;
  bool insideData(double lat, double lon) const;

  const std::map<std::string, std::unique_ptr<LoadedCell>>& cells() const { return loaded_; }
  int residentCount() const { return static_cast<int>(loaded_.size()); }
  int knownCount() const { return static_cast<int>(meta_.cells.size()); }
  int pendingJobs() const { return static_cast<int>(jobs_.size()); }
  size_t buildingCount() const;
  std::optional<double> minTerrainZ() const;

  // Verified interiors (streamed near their entrances via rj::stream::InteriorStreamer).
  const Interior* interior(const std::string& id) const;
  const std::map<std::string, std::unique_ptr<Interior>>& interiors() const { return interiors_; }
  bool forceLoadInterior(const std::string& id);

 private:
  void requestLoad(const rj::stream::StreamKey& key) override;
  void requestUnload(const rj::stream::StreamKey& key) override;
  void loadInterior(const std::string& id) override;
  void unloadInterior(const std::string& id) override;
  void placeCell(LoadedCell& c);
  void rebuildHash();

  std::filesystem::path dir_;
  SliceMeta meta_;
  std::unique_ptr<rj::geo::FloatingOrigin> origin_;
  std::unique_ptr<rj::stream::HierarchicalStreamer> streamer_;
  double current_view_ = 0;

  struct Job {
    rj::stream::StreamKey key;
    std::future<std::unique_ptr<CellCpu>> fut;
    bool cancelled = false;
  };
  std::vector<Job> jobs_;
  std::vector<rj::stream::StreamKey> empty_completions_;
  std::map<std::string, std::unique_ptr<LoadedCell>> loaded_;
  std::map<std::string, std::unique_ptr<Interior>> interiors_;
  std::unique_ptr<rj::stream::InteriorStreamer> interior_streamer_;
  std::vector<rj::stream::InteriorCandidate> interior_candidates_;

  static constexpr double kBucket = 25.0;
  std::unordered_map<int64_t, std::vector<std::pair<const LoadedCell*, int>>> hash_;
};

}  // namespace rjc
