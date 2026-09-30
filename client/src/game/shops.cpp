#include "game/shops.hpp"

#include <cmath>
#include <sstream>

#include "platform/paths.hpp"
#include "world/world.hpp"

namespace rjc {

bool Shops::load(const std::filesystem::path& file, std::string& err) {
  const auto text = readText(file);
  if (!text) {
    err = "cannot open " + file.string();
    return false;
  }
  std::istringstream in(*text);
  spots_.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string tag;
    ShopSpot s;
    double z = 0;
    if (!(ls >> tag) || tag != "shop") continue;
    if (!(ls >> s.kind >> s.counter_geo.lat_deg >> s.counter_geo.lon_deg >> z >> s.stand_geo.lat_deg >> s.stand_geo.lon_deg)) continue;
    s.counter_geo.h_ellipsoidal_m = s.stand_geo.h_ellipsoidal_m = z;  // (height above the sea, as the stations)
    spots_.push_back(s);
  }
  return true;
}

void Shops::place(const World& world) {
  if (!world.hasOrigin()) return;
  for (auto& s : spots_) {
    s.counter = world.toLocal(s.counter_geo);
    s.stand = world.toLocal(s.stand_geo);
  }
}

int Shops::near(const rj::geo::Vec3d& p, double r) const {
  int best = -1;
  double bd = r;
  for (size_t i = 0; i < spots_.size(); ++i) {
    const auto& s = spots_[i];
    const double d = std::hypot(s.stand.x - p.x, s.stand.y - p.y);
    if (d < bd && std::fabs(s.stand.z - p.z) < 1.2) {
      bd = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

const std::vector<ShopItem>& Shops::menu(const std::string& kind) {
  // (game values, typical prices in yen)
  static const std::vector<ShopItem> konbini = {{"onigiri", 160}, {"sandwich", 330}, {"bento", 560},
                                                {"green_tea", 150}, {"coffee_can", 140}, {"ice_cream", 180}};
  static const std::vector<ShopItem> cafe = {{"blend", 420}, {"latte", 480}, {"tea", 450}, {"cake", 520}, {"toast", 380}};
  static const std::vector<ShopItem> general = {{"umbrella", 700}, {"flashlight", 980}, {"batteries", 420},
                                                {"notebook", 210}, {"towel", 600},      {"toothbrush", 260}};
  if (kind == "konbini") return konbini;
  if (kind == "cafe") return cafe;
  return general;
}

}  // namespace rjc
