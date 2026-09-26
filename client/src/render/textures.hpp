#pragma once
// Procedural, tileable detail textures generated at start-up (no third-party image assets):
//   noise    RGBA: r low-frequency fbm, g second fbm (puddles / cloud thickness), b fine fbm,
//                  a repair-patch mask (asphalt patching)
//   asphalt  RGBA: rgb albedo tint (~0.5 = neutral), a roughness       (tile 3.5 m)
//   asphaltN RGBA: rg normal xy, b height, a cavity (cracks / voids)
//   paving   RGBA: rgb linear albedo of 30 cm sidewalk tiles, a roughness (tile 1.2 m)
//   pavingN  RGBA: rg normal xy (joints), b height, a cavity

#include "raylib.h"

namespace rjc {

struct DetailTextures {
  Texture2D noise{}, asphalt{}, asphaltN{}, paving{}, pavingN{};
  bool generate(int size = 512);
  void unload();
};

}  // namespace rjc
