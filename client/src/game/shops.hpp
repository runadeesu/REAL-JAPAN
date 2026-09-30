#pragma once
// Walk-in shops of the fictional country (data/world/country/shops.txt, written by the cook): where
// each shop's counter is and where a customer stands at it, and what the three generic kinds of
// shop sell. Goods and prices are game values (typical of Japanese shops, no brands or names).

#include <filesystem>
#include <string>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;

struct ShopSpot {
  std::string kind;              // konbini | cafe | general
  rj::geo::Geodetic counter_geo, stand_geo;
  rj::geo::Vec3d counter, stand;  // origin ENU (floor level)
};

struct ShopItem {
  const char* key;  // lang key suffix: shop.item.<key>
  int yen;
};

class Shops {
 public:
  bool load(const std::filesystem::path& file, std::string& err);
  bool loaded() const { return !spots_.empty(); }
  void place(const World& world);
  const std::vector<ShopSpot>& spots() const { return spots_; }
  // the shop whose customer spot is within `r` of p (and about the same floor), or -1
  int near(const rj::geo::Vec3d& p, double r) const;
  static const std::vector<ShopItem>& menu(const std::string& kind);

 private:
  std::vector<ShopSpot> spots_;
};

}  // namespace rjc
