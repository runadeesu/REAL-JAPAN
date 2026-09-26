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
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"
#include "rj/stream/streamer.hpp"
#include "world/cell.hpp"

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
  std::vector<CellMeta> cells;
  std::vector<Poi> pois;
  std::vector<SourceMeta> sources;
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
};

class World : public rj::stream::ICellIO {
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

 private:
  void requestLoad(const rj::stream::StreamKey& key) override;
  void requestUnload(const rj::stream::StreamKey& key) override;
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

  static constexpr double kBucket = 25.0;
  std::unordered_map<int64_t, std::vector<std::pair<const LoadedCell*, int>>> hash_;
};

}  // namespace rjc
