#pragma once
// Procedural trees and hedges for real PLATEAU vegetation objects (veg: SolitaryVegetationObject
// position / height / crown radius, PlantCover footprints). The species is not in the data, so a
// generic broadleaf street tree is generated (trunk, branches, leaf cards around the real crown).
// Leaf texture is generated at start-up (no third-party image assets).

#include "raylib.h"

namespace rjc {

struct TreeMeshes {
  Mesh bark{};
  Mesh leaves{};
};

// Mesh coordinates are cell-local ENU relative to the tree base (z up), like other detail geometry.
TreeMeshes buildTree(unsigned seed, float height, float crown_radius);
// Hedge from horizontal footprint triangles (cell ENU, 9 floats each) -> mesh in cell ENU.
Mesh buildHedge(const float* tris, int ntris, float hedge_height);
Texture2D generateLeafTexture(int size);

}  // namespace rjc
