// The player's flat: a one-room flat (1K, furnished; fictional) on the ground floor of a building in
// the capital, not far from the start (home.txt, from the pipeline). It is rented from the phone's
// "flat" app - the rent (a game value) comes out of the wallet every 30 game days, and a rent that
// cannot be paid ends the lease. Until it is rented the front door is shut and locked; once it is,
// the door stands open and the bed sleeps the night through to 7:00 (or a two-hour nap in the day):
// hunger and thirst fall at half the waking rate, the clothes dry, and the game is saved.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "app.hpp"
#include "platform/paths.hpp"
#include "ui/ui.hpp"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {
constexpr int64_t kRentYen = 65000;          // a month (a game value)
constexpr int64_t kLease = 30 * 86400;       // one payment covers 30 game days
}  // namespace

void App::loadHome(const std::filesystem::path& file) {
  home_ = HomeSpot{};
  std::istringstream in(readText(file).value_or(""));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string tag;
    double z = 0;
    HomeSpot h;
    if (!(ls >> tag >> h.bed_geo.lat_deg >> h.bed_geo.lon_deg >> z >> h.stand_geo.lat_deg >> h.stand_geo.lon_deg >> h.door_a_geo.lat_deg >>
          h.door_a_geo.lon_deg >> h.door_b_geo.lat_deg >> h.door_b_geo.lon_deg) ||
        tag != "home")
      continue;
    for (auto* g : {&h.bed_geo, &h.stand_geo, &h.door_a_geo, &h.door_b_geo}) g->h_ellipsoidal_m = z;
    h.ok = true;
    home_ = h;
    break;
  }
}

void App::placeHome() {
  if (!home_.ok || !world_.hasOrigin()) return;
  home_.bed = world_.toLocal(home_.bed_geo);
  home_.stand = world_.toLocal(home_.stand_geo);
  home_.door_a = world_.toLocal(home_.door_a_geo);
  home_.door_b = world_.toLocal(home_.door_b_geo);
}

bool App::playerOutsideHome() const {
  // the doorway's line: the bed is on the inside of it
  const auto side = [&](const rj::geo::Vec3d& p) {
    return (home_.door_b.x - home_.door_a.x) * (p.y - home_.door_a.y) - (home_.door_b.y - home_.door_a.y) * (p.x - home_.door_a.x);
  };
  return side(player_.pos) * side(home_.bed) <= 0.0;
}

void App::addHomeDoorWall(std::vector<float>& walls) const {
  if (!homeLocked() || !playerOutsideHome()) return;  // (someone inside when the lease ends can still walk out)
  const auto& a = home_.door_a;
  const auto& b = home_.door_b;
  if (std::hypot(a.x - player_.pos.x, a.y - player_.pos.y) > 60.0) return;
  walls.insert(walls.end(), {static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(b.x), static_cast<float>(b.y),
                             static_cast<float>(a.z - 0.3), static_cast<float>(a.z + 2.1)});
}

bool App::rentHome() {
  if (!home_.ok || life_.has_home) return false;
  if (!ledger_ || ledger_->transfer(player_account_, ledger_->externalAccount(), kRentYen, rj::econ::TxCategory::Rent, clock_.unixUtc(),
                                    tr("home.rent_memo")) != rj::econ::TxResult::Ok) {
    toast(tr("money.short"));
    return false;
  }
  life_.has_home = true;
  life_.rent_paid_until = clock_.unixUtc() + kLease;
  message(tr("msg.from.agent"), tr("home.rented"));
  autosave();
  return true;
}

void App::sleepAtHome() {
  const auto t = jst();
  const int m = t.hour * 60 + t.minute;
  const bool night = t.hour >= 18 || t.hour < 7;
  const int mins = night ? ((7 * 60 - m) + 1440) % 1440 : 120;  // to 7:00, or a two-hour nap
  const double h = std::max(1, mins) / 60.0;
  clock_.advanceGame(std::max(1, mins) * 60.0);
  // asleep the body needs about half as much (the waking rates are in updateLife)
  life_.hunger = std::max(0.0f, life_.hunger - static_cast<float>(3.0 * h));
  life_.thirst = std::max(0.0f, life_.thirst - static_cast<float>(4.5 * h));
  life_.wet = 0.0f;
  life_.last_unix = clock_.unixUtc();
  slept_until_ = clock_.unixUtc();
  toast(i18n_.f(night ? "home.slept" : "home.napped", {{"h", fixed(h, 1)}}));
  autosave();
}

void App::updateHome(float dt) {
  if (!home_.ok) return;
  // the rent, every 30 game days (a direct debit from the wallet)
  if (life_.has_home && clock_.unixUtc() >= life_.rent_paid_until) {
    if (ledger_ && ledger_->transfer(player_account_, ledger_->externalAccount(), kRentYen, rj::econ::TxCategory::Rent, clock_.unixUtc(),
                                     tr("home.rent_memo")) == rj::econ::TxResult::Ok) {
      life_.rent_paid_until += kLease;
      message(tr("msg.from.agent"), i18n_.f("home.rent_paid", {{"yen", withCommas(kRentYen)}}));
    } else {
      life_.has_home = false;
      message(tr("msg.from.agent"), tr("home.evicted"));
    }
  }
  // the door leaf swings open once the flat is rented
  home_door_open_ = std::clamp(home_door_open_ + (life_.has_home ? dt : -dt) * 1.5f, 0.0f, 1.0f);
  if (driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_ || player_.fly) return;
  const rj::geo::Vec3d dm{(home_.door_a.x + home_.door_b.x) / 2, (home_.door_a.y + home_.door_b.y) / 2, home_.door_a.z};
  if (homeLocked()) {
    if (std::hypot(dm.x - player_.pos.x, dm.y - player_.pos.y) < 2.2 && std::fabs(dm.z - player_.pos.z) < 1.5 && playerOutsideHome())
      prompt_ = tr("home.locked");
    return;
  }
  if (aim_icon_ != AimIcon::None) return;
  // the bed: from beside it, looking at it
  if (std::hypot(home_.bed.x - player_.pos.x, home_.bed.y - player_.pos.y) > 2.4 || std::fabs(home_.bed.z - player_.pos.z) > 1.2) return;
  if (!aimAt({home_.bed.x, home_.bed.y, home_.bed.z + 0.45}, 3.2, 55.0)) return;
  const auto t = jst();
  aim_icon_ = AimIcon::Hand;
  aim_label_ = tr(t.hour >= 18 || t.hour < 7 ? "aim.sleep" : "aim.nap");
  if (usePressed()) sleepAtHome();
}

void App::drawHomeDoor(const Camera3D& cam) {
  if (!home_.ok) return;
  const rj::geo::Vec3d cp = rlToEnu(cam.position);
  const auto& a = home_.door_a;
  const auto& b = home_.door_b;
  if (std::hypot(a.x - cp.x, a.y - cp.y) > 150.0) return;
  // hinged at one end; it opens inwards (towards the bed's side of the doorway)
  const double len = std::hypot(b.x - a.x, b.y - a.y);
  const double hd = std::atan2(b.x - a.x, b.y - a.y);
  const double cross = (b.x - a.x) * (home_.bed.y - a.y) - (b.y - a.y) * (home_.bed.x - a.x);
  const double in = cross > 0 ? -1.0 : 1.0;  // (compass heading: + is clockwise)
  const double th = hd + in * home_door_open_ * 95.0 * DEG2RAD;
  const double w = len - 0.06;
  const rj::geo::Vec3d c{a.x + std::sin(th) * (0.03 + w / 2), a.y + std::cos(th) * (0.03 + w / 2), a.z + 1.02};
  renderer_.drawBox(c, static_cast<float>(th), {0.022f, static_cast<float>(w / 2), 1.02f}, 0, Color{96, 78, 62, 255});
  // the handle, on the side away from the hinge
  const rj::geo::Vec3d hnd{a.x + std::sin(th) * (w - 0.08), a.y + std::cos(th) * (w - 0.08), a.z + 1.0};
  renderer_.drawBox(hnd, static_cast<float>(th), {0.05f, 0.02f, 0.06f}, 0, Color{200, 196, 186, 255});
}

void App::drawHomeOnMap(const std::function<Vector2(const rj::geo::Vec3d&)>& toScreen, double half) {
  if (!home_.ok) return;
  const Vector2 s = toScreen(home_.bed);
  const float k = 7 * ui_.scale();
  const Color c = life_.has_home ? Color{80, 200, 120, 255} : Color{200, 200, 200, 255};
  DrawRectangleV({s.x - k, s.y - k * 0.4f}, {2 * k, 1.4f * k}, c);
  DrawTriangle({s.x - k * 1.3f, s.y - k * 0.4f}, {s.x + k * 1.3f, s.y - k * 0.4f}, {s.x, s.y - k * 1.6f}, c);
  DrawRectangleLinesEx({s.x - k, s.y - k * 0.4f, 2 * k, 1.4f * k}, 1.5f, BLACK);
  if (half < 1500.0) ui_.text(tr(life_.has_home ? "home.map_mine" : "home.map_vacant"), s.x / ui_.scale() + 12, s.y / ui_.scale() - 12, 18, c);
}

void App::drawPhoneFlat(float cx, float yy, float cw) {
  ui_.text(tr("phone.flat"), cx, yy, 32, theme::kText);
  yy += 52;
  if (!home_.ok) {
    ui_.textWrapped(tr("home.none"), cx, yy, cw, 22, theme::kMuted);
    return;
  }
  ui_.text(tr("home.title"), cx, yy, 26, theme::kText);
  yy += 40;
  const double d = std::hypot(home_.bed.x - player_.pos.x, home_.bed.y - player_.pos.y);
  ui_.text(i18n_.f("home.distance", {{"m", withCommas(static_cast<int64_t>(std::lround(d / 10.0) * 10))}}), cx, yy, 22, theme::kMuted);
  yy += 34;
  ui_.text(i18n_.f("home.rent", {{"yen", withCommas(kRentYen)}}), cx, yy, 24, theme::kText);
  yy += 44;
  if (life_.has_home) {
    const auto due = rj::sim::GameClock(life_.rent_paid_until).jst();
    ui_.text(i18n_.f("home.leased", {{"m", std::to_string(due.date.m)}, {"d", std::to_string(due.date.d)}}), cx, yy, 22, theme::kGood);
    yy += 40;
    if (ui_.button({cx, yy, cw, 54}, tr("home.leave"), true, 24.0f)) {
      life_.has_home = false;
      toast(tr("home.left"));
    }
    yy += 70;
  } else {
    const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
    if (ui_.button({cx, yy, cw, 58}, i18n_.f("home.take", {{"yen", withCommas(kRentYen)}}), bal >= kRentYen, 24.0f)) rentHome();
    yy += 74;
  }
  yy += ui_.textWrapped(tr("home.help"), cx, yy, cw, 20, theme::kMuted) + 12;
  ui_.textWrapped(tr("home.note"), cx, yy, cw, 20, theme::kWarn);
}

}  // namespace rjc
