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
    toast(tr("rail.no_money"));
    return false;
  }
  ++inventory_[it.key];
  toast(i18n_.f("shop.bought", {{"item", name}, {"yen", withCommas(it.yen)}}));
  return true;
}

void App::updateShopActions() {
  if (!shops_.loaded()) return;
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
    const double th = t.heading * DEG2RAD, dx = v.pos.x - t.pos.x, dy = v.pos.y - t.pos.y;
    const double along = dx * std::sin(th) + dy * std::cos(th), across = dx * std::cos(th) - dy * std::sin(th);
    if (std::fabs(along) > 4.5 || std::fabs(across) > t.width / 2 + 1.5 || std::fabs(v.pos.z - t.pos.z) > 4.0) continue;
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
                                       tr("toll.company")) == rj::econ::TxResult::Ok)
        toast(i18n_.f("toll.exit", {{"ic", t.name}, {"fare", withCommas(fare)}}));
      else
        toast(tr("rail.no_money"));
      toll_entry_ = -1;
    }
    return;
  }
}

}  // namespace rjc
