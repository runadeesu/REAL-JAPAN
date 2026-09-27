#pragma once
// RJCELL v1 loading (format defined in pipeline/realjapan_pipeline/rjcell.py).
// Parsing + CPU preparation run on a worker thread; GPU upload runs on the
// main thread (OpenGL context).

#include <cstdint>
#include <string>
#include <vector>

#include "raylib.h"
#include "rj/geo/local_frame.hpp"
#include "world/detail.hpp"

namespace rjc {

// RJCELL chunk page for procedural facades (fictional island): uv = facade metres / (2048, 1024),
// vertex alpha = facade style (see facade() in render/shaders.hpp).
constexpr int kPageProcedural = -2;

struct BuildingInfo {
  std::string id;
  std::string name;
  uint16_t usage = 0;
  uint16_t bclass = 0;
  float measured_height = -1.0f;
  int16_t storeys_above = -1;
  int16_t storeys_below = -1;
  uint8_t lod = 0;
  uint8_t geometry_status = 0;  // 0 = VERIFIED_EXTERIOR
  uint8_t interior_status = 2;  // 2 = UNKNOWN
  uint32_t source_index = 0;
  float ground_z = 0.0f;
  float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};  // cell-local ENU
  uint32_t fp_first = 0, fp_count = 0;
  int32_t chunk = -1;     // first render chunk of the building
  bool textured = false;  // faces carry PLATEAU photo textures (real facade imagery)
};

struct CellCpu {
  std::string mesh;
  double anchor[3] = {0, 0, 0};  // lat, lon, h
  double bounds[4] = {0, 0, 0, 0};
  std::vector<BuildingInfo> buildings;
  std::vector<float> footprints;  // x,y pairs (cell ENU)
  struct Chunk {
    int page = -1;  // atlas page index, -1 = vertex colour only
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned char> col;
    std::vector<unsigned short> idx;
  };
  std::vector<Chunk> chunks;
  int tnx = 0, tny = 0;
  double tlat0 = 0, tlon0 = 0, tdlat = 0, tdlon = 0;
  std::vector<float> theight;
  // Terrain mesh prepared on the worker thread (cell ENU).
  std::vector<float> tpos, tnrm, tuv;
  std::vector<unsigned short> tidx;
  Image ground{};  // decoded ground texture (CPU)
  std::vector<Image> pages;  // decoded photo atlases (CPU), uploaded then freed
  CellDetailCpu detail;      // street-level detail (RJDET), optional
  size_t bytes = 0;
};

bool parseCell(const std::vector<unsigned char>& data, CellCpu& out, std::string& err);
void prepareTerrain(CellCpu& c);  // builds tpos/tnrm/tuv/tidx from the height grid

struct CellGpu {
  std::vector<Mesh> chunks;
  std::vector<int> chunk_page;
  std::vector<Texture2D> pages;
  Mesh terrain{};
  Texture2D ground{};
  CellDetailGpu detail;
  bool uploaded = false;
};

void uploadCell(CellCpu& cpu, CellGpu& gpu);  // frees the big CPU vertex arrays afterwards
void unloadCell(CellGpu& gpu);

}  // namespace rjc
