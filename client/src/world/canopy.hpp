#pragma once
// Forest canopy of a cell in the fictional country, built on the loader thread from the cell's
// land-cover map (forest weight) over its terrain grid: a bumpy crown surface 13-22 m above the
// ground (crown heights vary with smooth noise and per point), dropping to the ground at the
// forest's edge. Carriageways, rail beds and water seen in the ground texture stay open.

#include "world/cell.hpp"

namespace rjc {

// Appends a canopy chunk (material kMatCanopy) to c.detail.chunks when the cell has land cover.
void buildCanopy(CellCpu& c);

}  // namespace rjc
