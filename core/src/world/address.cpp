#include "rj/world/address.hpp"

namespace rj::world {

AddressLevel WorldAddress::depth() const {
  if (!interior_id.empty()) return AddressLevel::Interior;
  if (!building_id.empty()) return AddressLevel::Building;
  if (cell) return AddressLevel::Cell;
  if (!district.empty()) return AddressLevel::District;
  if (municipality) return AddressLevel::Municipality;
  if (prefecture) return AddressLevel::Prefecture;
  if (region) return AddressLevel::Region;
  return AddressLevel::Japan;
}

bool WorldAddress::contains(const WorldAddress& o) const {
  if (region && o.region != region) return false;
  if (prefecture && o.prefecture != prefecture) return false;
  if (municipality && o.municipality != municipality) return false;
  if (!district.empty() && o.district != district) return false;
  if (cell && o.cell != cell) return false;
  if (!building_id.empty() && o.building_id != building_id) return false;
  if (!interior_id.empty() && o.interior_id != interior_id) return false;
  return true;
}

std::string WorldAddress::toString() const {
  std::string s = "JP";
  if (region) s += "/" + std::string(regionNameEn(*region));
  if (prefecture) s += "/" + std::to_string(prefecture);
  if (municipality) s += "/" + std::to_string(municipality);
  if (!district.empty()) s += "/" + district;
  if (cell) s += "/m" + cell->str();
  if (!building_id.empty()) s += "/" + building_id;
  if (!interior_id.empty()) s += "/" + interior_id;
  return s;
}

WorldAddress WorldAddress::forMunicipality(uint32_t code5) {
  WorldAddress a;
  a.prefecture = static_cast<uint8_t>(code5 / 1000);
  if (auto p = prefectureByCode(a.prefecture)) a.region = p->region;
  a.municipality = code5;
  return a;
}

}  // namespace rj::world
