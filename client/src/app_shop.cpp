// Walk-in shops: stand at the counter, look at it and press E (or click) for the menu; buy with the
// number keys or by clicking an item. Goods and prices are game values; what is bought is kept in
// the save (a simple inventory, shown in the phone's wallet).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "app.hpp"
#include "platform/paths.hpp"
#include "ui/ui.hpp"
#include "world/coords.hpp"
#include "util/text.hpp"

namespace rjc {

std::string App::inventoryString() const {
  std::string s;
  for (const auto& [k, n] : inventory_) {
    if (n <= 0) continue;
    if (!s.empty()) s += ';';
    s += k + ":" + std::to_string(n);
  }
  return s;
}

void App::parseInventory(const std::string& s) {
  inventory_.clear();
  std::istringstream in(s);
  std::string part;
  while (std::getline(in, part, ';')) {
    const auto c = part.find(':');
    if (c == std::string::npos || c == 0) continue;
    const int n = std::atoi(part.c_str() + c + 1);
    if (n > 0) inventory_[part.substr(0, c)] = n;
  }
}

bool App::buyItem(const ShopItem& it) {
  const std::string name = tr(std::string("shop.item.") + it.key);
  if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), it.yen, rj::econ::TxCategory::Purchase, clock_.unixUtc(), name) !=
                      rj::econ::TxResult::Ok) {
    toast(tr("money.short"));
    return false;
  }
  ++inventory_[it.key];
  if (shop_open_ >= 0) ++shop_visits_[shop_open_];  // (the clerk gets to know a regular)
  toast(i18n_.f("shop.bought", {{"item", name}, {"yen", withCommas(it.yen)}}) + "  " + tr("shop.thanks"));
  return true;
}

void App::drawShopClerks(const Camera3D& cam) {
  // a clerk behind every counter near the camera, facing the customer's spot (in the shop's colours)
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  for (size_t i = 0; i < shops_.spots().size(); ++i) {
    const ShopSpot& sp = shops_.spots()[i];
    if (std::hypot(sp.counter.x - cp.x, sp.counter.y - cp.y) > 60.0) continue;
    const double dx = sp.stand.x - sp.counter.x, dy = sp.stand.y - sp.counter.y, d = std::max(0.1, std::hypot(dx, dy));
    const rj::geo::Vec3d feet{sp.counter.x - dx / d * 0.75, sp.counter.y - dy / d * 0.75, sp.counter.z};
    const Color shirt = sp.kind == "konbini" ? Color{40, 110, 175, 255} : sp.kind == "cafe" ? Color{70, 48, 34, 255} : Color{176, 64, 52, 255};
    renderer_.drawStandingPerson(enuToRl(feet), static_cast<float>(std::atan2(dx, dy)), static_cast<int>(i % 5), shirt, Color{36, 36, 42, 255});
  }
}

void App::updateShopActions() {
  if (!shops_.loaded()) return;
  // the clerk greets whoever comes up to the counter (words on the screen; there are no voices)
  {
    const int k = shops_.near(player_.pos, 3.5);
    if (k >= 0 && k != shop_greeted_) toast(tr(shop_visits_[k] >= 3 ? "shop.welcome_regular" : "shop.welcome"));
    if (k >= 0) shop_greeted_ = k;
    else if (shop_greeted_ >= 0 && shops_.near(player_.pos, 12.0) != shop_greeted_) shop_greeted_ = -1;
  }
  static const bool test = std::getenv("RJ_SHOP_TEST") != nullptr;  // test aid: open the nearest counter's menu
  if (shop_open_ >= 0) {
    // the menu: E closes it, walking away closes it, number keys buy
    const ShopSpot& sp = shops_.spots()[static_cast<size_t>(shop_open_)];
    if (std::hypot(sp.stand.x - player_.pos.x, sp.stand.y - player_.pos.y) > (test ? 9.0 : 2.5) || IsKeyPressed(KEY_E)) {
      shop_open_ = -1;
      return;
    }
    const auto& items = Shops::menu(sp.kind);
    for (size_t i = 0; i < items.size() && i < 9; ++i)
      if (IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + static_cast<int>(i)))) buyItem(items[i]);
    return;
  }
  if (driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_ || player_.fly) return;
  if (test) {
    if (const int t = shops_.near(player_.pos, 8.0); t >= 0) shop_open_ = t;
    if (shop_open_ >= 0) return;
  }
  if (aim_icon_ != AimIcon::None) return;  // the crosshair is on something else
  const int k = shops_.near(player_.pos, 1.3);
  if (k < 0) return;
  const ShopSpot& sp = shops_.spots()[static_cast<size_t>(k)];
  if (!aimAt({sp.counter.x, sp.counter.y, sp.counter.z + 1.0}, 4.0, 45.0)) return;
  aim_icon_ = AimIcon::Hand;
  aim_label_ = tr("aim.shop." + sp.kind);
  if (usePressed()) shop_open_ = k;
}

void App::drawShopMenu() {
  if (shop_open_ < 0 || shop_open_ >= static_cast<int>(shops_.spots().size())) return;
  const ShopSpot& sp = shops_.spots()[static_cast<size_t>(shop_open_)];
  const auto& items = Shops::menu(sp.kind);
  const float w = 720, x = (ui_.vw() - w) / 2, row = 64;
  const float h = 240 + row * static_cast<float>(items.size());
  float y = 540 - h / 2;
  ui_.panel({x, y, w, h}, Color{16, 18, 22, 225});
  ui_.text(tr("shop.name." + sp.kind), x + 30, y + 22, 34, theme::kText);
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  ui_.textRight(i18n_.f("phone.wallet_balance", {{"n", withCommas(bal)}}), x + w - 30, y + 30, 24, theme::kMuted);
  y += 80;
  for (size_t i = 0; i < items.size(); ++i) {
    const ShopItem& it = items[i];
    const std::string name = tr(std::string("shop.item.") + it.key);
    const auto have = inventory_.find(it.key);
    std::string label = std::to_string(i + 1) + "  " + name + "  ¥" + withCommas(it.yen);
    if (have != inventory_.end() && have->second > 0) label += "  (" + i18n_.f("shop.have", {{"n", std::to_string(have->second)}}) + ")";
    if (ui_.button({x + 24, y, w - 48, row - 8}, label, bal >= it.yen, 26.0f)) buyItem(it);
    y += row;
  }
  ui_.text(tr("shop.hint"), x + 30, y + 18, 22, theme::kMuted);
  if (ui_.button({x + w - 224, y + 58, 200, 52}, tr("shop.close"), true, 26.0f)) shop_open_ = -1;  // (touch: no E key)
}

void App::loadTolls(const std::filesystem::path& file) {
  tolls_.clear();
  std::istringstream in(readText(file).value_or(""));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string tag;
    TollPlaza t;
    if (!(ls >> tag >> t.name >> t.geo.lat_deg >> t.geo.lon_deg >> t.geo.h_ellipsoidal_m >> t.heading >> t.width) || tag != "toll") continue;
    tolls_.push_back(t);
  }
}

App::TollLane App::tollLane(const TollPlaza& t, int lane) const {
  // lane 0 keeps left going along the plaza's heading, lane 1 the other way (left-hand traffic);
  // the bar stands at the lane's exit end of the islands, pivoting on the outer island
  const double th = t.heading * DEG2RAD;
  const double dx = std::sin(th), dy = std::cos(th), nx = std::cos(th), ny = -std::sin(th);
  const double s = lane == 0 ? 1.0 : -1.0;
  const double outer = t.width / 2 + 0.7 - 0.62, inner = 0.62;
  const double along = s * 3.9;
  TollLane L;
  L.pivot = {t.pos.x + dx * along - nx * s * outer, t.pos.y + dy * along - ny * s * outer, t.pos.z + 1.0};
  L.tip = {t.pos.x + dx * along - nx * s * inner, t.pos.y + dy * along - ny * s * inner, t.pos.z + 1.0};
  L.arm_hd = std::atan2(L.tip.x - L.pivot.x, L.tip.y - L.pivot.y) / DEG2RAD;
  L.along = lane == 0 ? t.heading : t.heading + 180.0;
  return L;
}

void App::updateTollBars(float dt, std::vector<float>& walls) {
  for (size_t i = 0; i < tolls_.size(); ++i) {
    TollPlaza& t = tolls_[i];
    if (std::hypot(t.pos.x - player_.pos.x, t.pos.y - player_.pos.y) > 400.0) continue;
    for (int lane = 0; lane < 2; ++lane) {
      const TollLane L = tollLane(t, lane);
      const double th = L.along * DEG2RAD, dx = std::sin(th), dy = std::cos(th);
      const double bx = (L.pivot.x + L.tip.x) / 2, by = (L.pivot.y + L.tip.y) / 2;
      // a car in the lane coming up to the bar (ETC reads its card 30 m ahead) lifts it
      auto coming = [&](const rj::geo::Vec3d& p) {
        const double a = (p.x - bx) * dx + (p.y - by) * dy, c = (p.x - bx) * dy - (p.y - by) * dx;
        return a > -30.0 && a < 4.0 && std::fabs(c) < 2.6 && std::fabs(p.z - t.pos.z) < 4.0;
      };
      bool want = false;
      for (const auto& v : traffic_.vehicles()) want = want || coming(v.pos);
      if (driving_.active() && coming(driving_.car().pos) && !t.blocked) want = true;
      if (t.blocked) want = false;
      t.bar[lane] = std::clamp(t.bar[lane] + (want ? 1.6f : -0.8f) * dt, 0.0f, 1.0f);
      if (t.bar[lane] < 0.5f)  // down: the arm across the lane stops cars and people
        walls.insert(walls.end(), {static_cast<float>(L.pivot.x), static_cast<float>(L.pivot.y), static_cast<float>(L.tip.x), static_cast<float>(L.tip.y),
                                   static_cast<float>(t.pos.z + 0.35), static_cast<float>(t.pos.z + 1.3)});
    }
    if (t.blocked && (!driving_.active() || std::hypot(t.pos.x - driving_.car().pos.x, t.pos.y - driving_.car().pos.y) > 35.0)) t.blocked = false;
  }
}

void App::drawTollBars(const Camera3D& cam) {
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  for (const TollPlaza& t : tolls_) {
    if (std::hypot(t.pos.x - cp.x, t.pos.y - cp.y) > 350.0) continue;
    for (int lane = 0; lane < 2; ++lane) {
      const TollLane L = tollLane(t, lane);
      const double hd = L.arm_hd * DEG2RAD;
      const double up = t.bar[lane] * 80.0 * DEG2RAD;
      const double len = std::hypot(L.tip.x - L.pivot.x, L.tip.y - L.pivot.y);
      const double fx = std::sin(hd) * std::cos(up), fy = std::cos(hd) * std::cos(up), fz = std::sin(up);
      // the bar machine on the island, then the arm in yellow and black
      renderer_.drawBox({L.pivot.x, L.pivot.y, t.pos.z + 0.5}, static_cast<float>(hd), {0.18f, 0.18f, 0.5f}, 0, Color{230, 190, 30, 255});
      const int n = 8;
      for (int k = 0; k < n; ++k) {
        const double m = (k + 0.5) * len / n;
        renderer_.drawBox({L.pivot.x + fx * m, L.pivot.y + fy * m, L.pivot.z + fz * m}, static_cast<float>(hd), {0.04f, static_cast<float>(len / n * 0.5), 0.05f}, 0,
                          k % 2 == 0 ? Color{240, 196, 20, 255} : Color{24, 24, 24, 255}, {0, 0, 0}, static_cast<float>(up));
      }
    }
  }
}

void App::updateTolls() {
  if (tolls_.empty() || !driving_.active()) return;
  const Vehicle& v = driving_.car();
  if (toll_last_ >= 0) {  // (clear of the last plaza before another one counts)
    const TollPlaza& t = tolls_[static_cast<size_t>(toll_last_)];
    if (std::hypot(t.pos.x - v.pos.x, t.pos.y - v.pos.y) > 30.0) toll_last_ = -1;
    return;
  }
  for (size_t i = 0; i < tolls_.size(); ++i) {
    const TollPlaza& t = tolls_[i];
    // read (and charged) on the way in, 5-16 m before the islands, so a refused card keeps the bar down
    const double th = t.heading * DEG2RAD, dx = v.pos.x - t.pos.x, dy = v.pos.y - t.pos.y;
    const double along = dx * std::sin(th) + dy * std::cos(th), across = dx * std::cos(th) - dy * std::sin(th);
    const double going = std::sin(v.yaw) * std::sin(th) + std::cos(v.yaw) * std::cos(th) >= 0 ? 1.0 : -1.0;
    const double ahead = -along * going;  // metres to the plaza centre in the car's direction
    if (ahead < 5.0 || ahead > 16.0 || std::fabs(across) > t.width / 2 + 1.5 || std::fabs(v.pos.z - t.pos.z) > 4.0) continue;
    toll_last_ = static_cast<int>(i);
    if (toll_entry_ < 0) {
      toll_entry_ = static_cast<int>(i);
      toast(i18n_.f("toll.enter", {{"ic", t.name}}));
    } else {
      // ETC-style fare (game values): a terminal charge plus about 25 yen per km between the plazas
      const TollPlaza& e = tolls_[static_cast<size_t>(toll_entry_)];
      const double km = std::hypot(t.pos.x - e.pos.x, t.pos.y - e.pos.y) * 1.15 / 1000.0;
      const int64_t fare = static_cast<int64_t>(std::lround((150.0 + 24.6 * km) / 10.0)) * 10;
      if (ledger_ && ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(),
                                       tr("toll.company")) == rj::econ::TxResult::Ok) {
        toast(i18n_.f("toll.exit", {{"ic", t.name}, {"fare", withCommas(fare)}}));
        toll_entry_ = -1;
      } else {
        toast(i18n_.f("toll.no_money", {{"fare", withCommas(fare)}}));  // the bar stays down (back out, earn, come again)
        tolls_[i].blocked = true;
        tolls_[i].bar[0] = tolls_[i].bar[1] = 0.0f;
      }
    }
    return;
  }
}

// Street vending machines (estimated positions, game/road_markings.cpp): look at one close by and
// press E for the drinks; they go into the bag like anything bought (prices are game values).
namespace {
const std::vector<ShopItem>& vendMenu() {
  static const std::vector<ShopItem> m = {{"green_tea", 160}, {"coffee_can", 130}, {"water", 110}, {"sports_drink", 160}};
  return m;
}
}  // namespace

void App::updateVending() {
  const auto& vs = markings_.vendings();
  if (vend_open_ >= 0) {
    if (vend_open_ >= static_cast<int>(vs.size())) {
      vend_open_ = -1;
      return;
    }
    const StreetVending& v = vs[static_cast<size_t>(vend_open_)];
    if (std::hypot(v.pos.x - player_.pos.x, v.pos.y - player_.pos.y) > 2.6 || IsKeyPressed(KEY_E)) {
      vend_open_ = -1;
      return;
    }
    const auto& items = vendMenu();
    for (size_t i = 0; i < items.size(); ++i)
      if (IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + static_cast<int>(i)))) buyItem(items[i]);
    return;
  }
  if (driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_ || player_.fly || aim_icon_ != AimIcon::None) return;
  static const bool test = std::getenv("RJ_VEND_TEST") != nullptr;  // test aid: open the nearest machine's menu
  int best = -1;
  double bd = test ? 400.0 : 1.8;
  for (size_t i = 0; i < vs.size(); ++i) {
    const double d = std::hypot(vs[i].pos.x - player_.pos.x, vs[i].pos.y - player_.pos.y);
    if (d < bd && std::fabs(vs[i].pos.z - player_.pos.z) < 1.5) {
      bd = d;
      best = static_cast<int>(i);
    }
  }
  if (best < 0) return;
  if (test) {
    if (std::getenv("RJ_VEND_LOOK")) {  // (and stand in front of it, looking at it)
      static bool once = false;
      if (!once) {
        once = true;
        const StreetVending& v = vs[static_cast<size_t>(best)];
        player_.pos = {v.pos.x + v.face.x * 2.8 + v.face.y * 0.9, v.pos.y + v.face.y * 2.8 - v.face.x * 0.9, v.pos.z};
        player_.snapToGround(world_);
        player_.yaw = static_cast<float>(std::atan2(-v.face.x - v.face.y * 0.3, -v.face.y + v.face.x * 0.3));
        player_.pitch = -0.12f;
      }
      return;
    }
    vend_open_ = best;
    return;
  }
  const StreetVending& v = vs[static_cast<size_t>(best)];
  if (!aimAt({v.pos.x - v.face.x * 0.35, v.pos.y - v.face.y * 0.35, v.pos.z + 1.1}, 3.0, 40.0)) return;
  aim_icon_ = AimIcon::Hand;
  aim_label_ = tr("aim.vending");
  if (usePressed()) vend_open_ = best;
}

void App::drawVendMenu() {
  if (vend_open_ < 0) return;
  const auto& items = vendMenu();
  const float w = 640, x = (ui_.vw() - w) / 2, row = 64;
  const float h = 230 + row * static_cast<float>(items.size());
  float y = 540 - h / 2;
  ui_.panel({x, y, w, h}, Color{16, 18, 22, 225});
  ui_.text(tr("vending.name"), x + 30, y + 22, 34, theme::kText);
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  ui_.textRight(i18n_.f("phone.wallet_balance", {{"n", withCommas(bal)}}), x + w - 30, y + 30, 24, theme::kMuted);
  y += 80;
  for (size_t i = 0; i < items.size(); ++i) {
    const ShopItem& it = items[i];
    const std::string label = std::to_string(i + 1) + "  " + tr(std::string("shop.item.") + it.key) + "  ¥" + withCommas(it.yen);
    if (ui_.button({x + 24, y, w - 48, row - 8}, label, bal >= it.yen, 26.0f)) buyItem(it);
    y += row;
  }
  ui_.text(tr("vending.hint"), x + 30, y + 18, 22, theme::kMuted);
  if (ui_.button({x + w - 224, y + 52, 200, 52}, tr("shop.close"), true, 26.0f)) vend_open_ = -1;
}

}  // namespace rjc
