#pragma once
// Hierarchical world address:
//   Japan > Region > Prefecture > Municipality > District > Cell > Building > Interior
//
// Administrative levels (Region..District) are *logical* partitions used by
// simulation LOD, weather, economy and data ownership. Physical streaming
// uses the JIS mesh grid (Cell). A building belongs to exactly one cell
// (the cell containing its footprint centroid).

#include <optional>
#include <string>

#include "rj/geo/mesh_code.hpp"
#include "rj/world/prefecture.hpp"

namespace rj::world {

enum class AddressLevel : uint8_t {
  Japan = 0,
  Region,
  Prefecture,
  Municipality,
  District,
  Cell,
  Building,
  Interior,
};

struct WorldAddress {
  std::optional<Region> region;
  uint8_t prefecture = 0;          // JIS X 0401, 0 = unset
  uint32_t municipality = 0;       // JIS X 0402 5-digit (e.g. 13113 = 渋谷区), 0 = unset
  std::string district;            // 町丁目 key (e.g. e-Stat small-area code), empty = unset
  std::optional<geo::MeshCode> cell;  // level-3 mesh
  std::string building_id;         // e.g. PLATEAU uro:buildingID
  std::string interior_id;         // floor/room/space id within the building

  AddressLevel depth() const;
  // True if `other` is this address or lies beneath it in the hierarchy.
  bool contains(const WorldAddress& other) const;
  std::string toString() const;

  static WorldAddress forMunicipality(uint32_t code5);
};

}  // namespace rj::world
