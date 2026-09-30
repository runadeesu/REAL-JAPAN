// The car being driven: fuel and damage. The tank empties with the distance and the throttle
// (about 480 km on a 40 L tank; game values); knocks against walls and other cars dent it and take
// power away, and a wrecked car will not go. Fuel stations (fuel.txt: in the towns and at the
// parking area) fill the tank and mend the car; the phone's road service brings 10 L or tows it
// to be mended anywhere, at a price. The car found in the street comes with some fuel in it.

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "app.hpp"
#include "platform/paths.hpp"
#include "ui/ui.hpp"
#include "util/text.hpp"

namespace rjc {
namespace {
constexpr double kTankL = 40.0;
constexpr int64_t kYenPerL = 175;  // (a game value)
}  // namespace

void App::loadFuelStations(const std::filesystem::path& file) {
  fuel_stations_.clear();
  std::istringstream in(readText(file).value_or(""));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string tag;
    FuelStation f;
    if (!(ls >> tag >> f.name >> f.geo.lat_deg >> f.geo.lon_deg >> f.geo.h_ellipsoidal_m) || tag != "fuel") continue;
    fuel_stations_.push_back(f);
  }
}

void App::onEnterCar(const Vehicle& v) {
  if (v.id == fuel_car_id_) return;  // (back into the same car)
  fuel_car_id_ = v.id;
  const uint32_t h = static_cast<uint32_t>(v.id) * 2654435761u;
  life_.fuel = 0.35f + 0.6f * static_cast<float>((h >> 8) % 1000u) / 1000.0f;
  life_.damage = 0.0f;
}

int64_t App::fuelCost() const { return static_cast<int64_t>(std::lround((1.0 - life_.fuel) * kTankL)) * kYenPerL; }
int64_t App::repairCost() const { return static_cast<int64_t>(std::lround(life_.damage * 180000.0 / 1000.0)) * 1000; }

void App::updateCar(float dt) {
  if (!driving_.hasCar()) return;
  if (driving_.active()) {
    const Vehicle& c = driving_.car();
    const float f0 = life_.fuel, d0 = life_.damage;
    // 12 km/L cruising, more under full throttle, a little at idle (engine running)
    if (driving_.engineRunning()) {
      const double km = std::fabs(c.v) * dt / 1000.0;
      const double litres = km / 12.0 * (0.6 + 0.9 * driving_.throttle()) + 0.8 / 3600.0 * dt;
      life_.fuel = std::max(0.0f, life_.fuel - static_cast<float>(litres / kTankL));
    }
    if (const float imp = driving_.takeDamageImpact(); imp > 2.5f) life_.damage = std::min(1.0f, life_.damage + (imp - 2.5f) / 45.0f);
    if (f0 >= 0.1f && life_.fuel < 0.1f) toast(tr("car.low_fuel"));
    if (f0 > 0.0f && life_.fuel <= 0.0f) toast(tr("car.no_fuel"));
    if (d0 < 0.5f && life_.damage >= 0.5f) toast(tr("car.damaged"));
    if (d0 < 1.0f && life_.damage >= 1.0f) toast(tr("car.wrecked"));
  }
  driving_.setEngine(1.0f - 0.55f * life_.damage, life_.fuel > 0.0f && life_.damage < 1.0f);
}

void App::updateFuelStation() {
  if (fuel_open_ >= 0) {
    const FuelStation& f = fuel_stations_[static_cast<size_t>(fuel_open_)];
    if (!driving_.active() || std::hypot(f.pos.x - driving_.car().pos.x, f.pos.y - driving_.car().pos.y) > 14.0 || IsKeyPressed(KEY_F)) fuel_open_ = -1;
    return;
  }
  if (!driving_.active() || std::fabs(driving_.car().v) > 1.5) return;
  const Vehicle& c = driving_.car();
  for (size_t i = 0; i < fuel_stations_.size(); ++i) {
    const FuelStation& f = fuel_stations_[i];
    if (std::hypot(f.pos.x - c.pos.x, f.pos.y - c.pos.y) > 11.0 || std::fabs(f.pos.z - c.pos.z) > 4.0) continue;
    prompt_ = tr("car.station_prompt");
    if (IsKeyPressed(KEY_F)) fuel_open_ = static_cast<int>(i);
    return;
  }
}

bool App::nearFuelStation() const {
  if (!driving_.active()) return false;
  const Vehicle& c = driving_.car();
  for (const auto& f : fuel_stations_)
    if (std::hypot(f.pos.x - c.pos.x, f.pos.y - c.pos.y) < 11.0 && std::fabs(f.pos.z - c.pos.z) < 4.0) return true;
  return false;
}

bool App::payCar(int64_t yen, const std::string& memo) {
  if (yen <= 0) return true;
  if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), yen, rj::econ::TxCategory::Purchase, clock_.unixUtc(), memo) !=
                      rj::econ::TxResult::Ok) {
    toast(tr("rail.no_money"));
    return false;
  }
  return true;
}

void App::drawFuelMenu() {
  if (fuel_open_ < 0 || fuel_open_ >= static_cast<int>(fuel_stations_.size())) return;
  const FuelStation& f = fuel_stations_[static_cast<size_t>(fuel_open_)];
  const float w = 720, h = 390, x = (ui_.vw() - w) / 2;
  float y = 540 - h / 2;
  ui_.panel({x, y, w, h}, Color{16, 18, 22, 225});
  ui_.text(tr("car.station") + "  " + f.name, x + 30, y + 22, 32, theme::kText);
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  ui_.textRight(i18n_.f("phone.wallet_balance", {{"n", withCommas(bal)}}), x + w - 30, y + 30, 24, theme::kMuted);
  y += 80;
  ui_.text(i18n_.f("car.status", {{"fuel", std::to_string(static_cast<int>(life_.fuel * 100))}, {"damage", std::to_string(static_cast<int>(life_.damage * 100))}}),
           x + 30, y, 24, theme::kText);
  y += 50;
  const int64_t fc = fuelCost(), rc = repairCost();
  if (ui_.button({x + 24, y, w - 48, 56}, i18n_.f("car.fill", {{"yen", withCommas(fc)}}), fc > 0 && bal >= fc, 26.0f) && payCar(fc, tr("car.fuel_memo"))) {
    life_.fuel = 1.0f;
    toast(tr("car.filled"));
  }
  y += 66;
  if (ui_.button({x + 24, y, w - 48, 56}, i18n_.f("car.repair", {{"yen", withCommas(rc)}}), rc > 0 && bal >= rc, 26.0f) && payCar(rc, tr("car.repair_memo"))) {
    life_.damage = 0.0f;
    toast(tr("car.repaired"));
  }
  y += 66;
  if (ui_.button({x + w - 224, y, 200, 52}, tr("shop.close"), true, 26.0f)) fuel_open_ = -1;
  ui_.text(tr("car.station_hint"), x + 30, y + 14, 20, theme::kMuted);
}

void App::drawCarPhone(float x, float& y, float w) {
  // (in the bag app) the car's state and the road service
  if (!driving_.hasCar()) return;
  ui_.text(i18n_.f("car.status", {{"fuel", std::to_string(static_cast<int>(life_.fuel * 100))}, {"damage", std::to_string(static_cast<int>(life_.damage * 100))}}), x, y,
           22, theme::kMuted);
  y += 34;
  if (life_.fuel < 0.25f && ui_.button({x, y, w, 48}, tr("car.service_fuel"), true, 22.0f) && payCar(5000, tr("car.service_memo"))) {
    life_.fuel = std::min(1.0f, life_.fuel + static_cast<float>(10.0 / kTankL));
    toast(tr("car.service_done"));
  }
  if (life_.fuel < 0.25f) y += 56;
  if (life_.damage > 0.0f) {
    const int64_t cost = 20000 + repairCost();
    if (ui_.button({x, y, w, 48}, i18n_.f("car.service_tow", {{"yen", withCommas(cost)}}), true, 22.0f) && payCar(cost, tr("car.service_memo"))) {
      life_.damage = 0.0f;
      toast(tr("car.repaired"));
    }
    y += 56;
  }
}

}  // namespace rjc
