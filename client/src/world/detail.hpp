#pragma once
// Street-level detail of a cell (RJDET v1, pipeline/realjapan_pipeline/streetdetail.py):
// raised sidewalks + curbs, road-marking decals, PLATEAU street furniture with materials,
// street-light heads and traffic-signal heads, crosswalk areas, ground contact occlusion.
// Parsing runs on the cell worker thread; GPU upload on the main thread.

#include <cstdint>
#include <string>
#include <vector>

#include "raylib.h"

namespace rjc {

// Material ids (must match MAT in streetdetail.py and the lit shader).
enum Mat : int {
  kMatDefault = 0,
  kMatCurb = 1,
  kMatSidewalk = 2,
  kMatTactile = 3,
  kMatMarking = 4,
  kMatMetal = 5,
  kMatMetalDark = 6,
  kMatSign = 7,
  kMatManhole = 8,
  kMatGrating = 9,
  kMatLamp = 10,
  kMatGlass = 11,
  kMatConcrete = 12,
  kMatFence = 13,
  kMatBronze = 14,
  kMatIsland = 15,
  kMatWater = 16,    // river / sea surface
  kMatCanopy = 17,   // forest canopy (fictional island mountains)
  kMatAsphalt = 18,  // bridge decks
  kMatBallast = 19,  // elevated track bed
  // Runtime-generated (procedural facade detail, vehicles, people): 20+
  kMatWindow = 20,
  kMatShopGlass = 21,
  kMatFrame = 22,
  kMatAwning = 23,
  kMatSignBand = 24,
  kMatAcUnit = 25,
  kMatBalcony = 26,
  kMatWallPaint = 27,
  kMatCarPaint = 28,
  kMatTyre = 29,
  kMatSignalLamp = 30,
  kMatCloth = 31,
  kMatSkin = 32,
  kMatFoliage = 33,
  kMatBark = 34,
  kMatUntinted = 35,  // vertex colour only (plates, liveries), not tinted per draw
  kMatClearGlass = 40,  // see-through glass (street detail): drawn after the opaque scene, blended
};

inline bool matIsFlat(int m) {
  return m == kMatSidewalk || m == kMatIsland || m == kMatMarking || m == kMatTactile || m == kMatManhole ||
         m == kMatGrating;
}

struct StreetLight {
  float pos[3];  // cell ENU
  float range;
  uint32_t kind;
};

struct SignalHead {
  float pos[3];  // cell ENU, head centre
  float axis_yaw, facing_yaw, length;  // compass radians
  uint32_t kind;                       // 0 vehicle (horizontal 3-lamp), 1 pedestrian
  int32_t group;                       // intersection cluster within the cell
  uint32_t phase;                      // 0/1 (two orthogonal phases, game assumption)
};

struct TreeRec {
  float base[3];  // cell ENU (on the terrain)
  float height, crown;
  uint32_t kind;
};

struct CellDetailCpu {
  struct Chunk {
    int mat = 0;
    bool indoor = false;  // (material id bit 8) inside a building: little sky light reaches it
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned char> col;
    std::vector<unsigned short> idx;
  };
  std::vector<Chunk> chunks;
  std::vector<float> walk;   // walkable triangles (cell ENU), 9 floats each
  std::vector<float> deck;   // bridge decks (RJDET extra walk section): drivable and walkable
  std::vector<float> cross;  // crosswalk triangles (cell ENU)
  std::vector<float> marks;  // centroids (x, y) of surveyed road-marking triangles (cell ENU)
  std::vector<StreetLight> lights;
  std::vector<SignalHead> signals;
  Image ao{};  // ground contact occlusion (grey), same UV as the ground texture
  std::vector<TreeRec> trees;   // PLATEAU veg SolitaryVegetationObject
  std::vector<float> hedges;    // PlantCover footprint triangles (cell ENU)
  // land cover (fictional country): RGBA weights of forest, rice paddy, upland field, bare ground,
  // same UV as the ground texture; the forest canopy is built from it on the loader thread
  Image landcover{};
  // collision walls (tagged WALL section): x0, y0, x1, y1, z low, z high per wall (cell ENU)
  std::vector<float> walls;
  bool present = false;
};

bool parseDetail(const std::vector<unsigned char>& file, CellDetailCpu& out, std::string& err);

struct CellDetailGpu {
  std::vector<Mesh> meshes;
  std::vector<int> mats;
  Texture2D ao{};
  struct Tree {
    Mesh bark{}, leaves{};
    float base[3];  // cell ENU
  };
  std::vector<Tree> trees;
  Mesh hedge{};
  Texture2D landcover{};
};

void uploadDetail(CellDetailCpu& cpu, CellDetailGpu& gpu);  // frees the CPU vertex arrays
void unloadDetail(CellDetailGpu& gpu);

}  // namespace rjc
