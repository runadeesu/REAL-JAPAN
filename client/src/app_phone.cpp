// The phone's other apps. Everything in them is the game's own: messages from the game's events,
// a phone book of fixed dialogues (no voices, no language model), the next trains worked out from
// the simulated trains (there is no timetable), a camera shortcut, a music player whose tunes are
// composed at run time, online shopping delivered to the flat's parcel box the next day, food
// delivered where you are (no courier is shown), a taxi that takes you somewhere while the game
// time runs (the ride itself is not shown), the airline's flights and a ticket bought ahead, a
// night in a station hotel (no rooms are shown) and a feed of fixed-template posts by the town's
// fictional residents. Prices and fares are game values.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app.hpp"
#include "rj/env/solar.hpp"
#include "rj/sim/npc.hpp"
#include "ui/ui.hpp"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

std::string two2(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }

std::string stamp(int64_t unix) {
  const auto t = rj::sim::GameClock(unix).jst();
  return std::to_string(t.date.m) + "/" + std::to_string(t.date.d) + " " + two2(t.hour) + ":" + two2(t.minute);
}

uint32_t mix(uint32_t a, uint32_t b) {
  uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

struct Catalog {
  const char* key;
  int yen, count;
};
// online shop (delivered to the flat) and food delivery (to where you are); game values
const Catalog kShop[] = {{"umbrella", 900, 1}, {"flashlight", 1180, 1}, {"batteries", 480, 1}, {"notebook", 260, 1},
                         {"towel", 700, 1},    {"toothbrush", 300, 1},  {"water", 780, 6},     {"sports_drink", 960, 6}};
const Catalog kFood[] = {{"gyudon", 650, 1}, {"ramen", 900, 1}, {"curry", 850, 1}, {"pizza", 1800, 1}};
constexpr int kShipping = 400, kDeliveryFee = 350, kHotelYen = 7800;

// taxi fare (the special wards' tariff the taxi job uses: first 1,096 m 500 yen, then 100 yen per 255 m;
// late night +20 %) for a road distance in metres
int64_t taxiFare(double m, int hour) {
  int64_t f = 500;
  if (m > 1096.0) f += 100 * static_cast<int64_t>(std::ceil((m - 1096.0) / 255.0));
  if (hour >= 22 || hour < 5) f = static_cast<int64_t>(std::llround(f * 1.2 / 10.0) * 10);
  return f;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// messages and the saved phone state

void App::message(const std::string& from, const std::string& text) {
  msgs_.push_back({clock_.unixUtc(), from, text});
  if (msgs_.size() > 60) msgs_.erase(msgs_.begin());
  ++msgs_unread_;
  toast(i18n_.f("msg.new", {{"from", from}}));
}

std::string App::phoneString() const {
  // records separated by '|', fields by '~' (both taken out of the texts)
  auto clean = [](std::string s) {
    for (char& c : s)
      if (c == '|' || c == '~' || c == '\n') c = ' ';
    return s;
  };
  std::string s;
  for (const auto& m : msgs_) s += "m~" + std::to_string(m.t) + "~" + clean(m.from) + "~" + clean(m.text) + "|";
  for (const auto& o : orders_) s += "o~" + std::to_string(o.due) + "~" + clean(o.item) + "~" + std::to_string(o.count) + "~" + (o.to_home ? "1" : "0") + "|";
  s += "u~" + std::to_string(msgs_unread_) + "|";
  if (flight_prepaid_) s += "f~1|";
  return s;
}

void App::parsePhone(const std::string& s) {
  msgs_.clear();
  orders_.clear();
  msgs_unread_ = 0;
  flight_prepaid_ = false;
  std::istringstream in(s);
  std::string rec;
  while (std::getline(in, rec, '|')) {
    std::vector<std::string> f;
    std::istringstream rs(rec);
    std::string x;
    while (std::getline(rs, x, '~')) f.push_back(x);
    if (f.empty()) continue;
    if (f[0] == "m" && f.size() >= 4) msgs_.push_back({std::atoll(f[1].c_str()), f[2], f[3]});
    else if (f[0] == "o" && f.size() >= 5) orders_.push_back({std::atoll(f[1].c_str()), f[2], std::atoi(f[3].c_str()), f[4] == "1"});
    else if (f[0] == "u" && f.size() >= 2) msgs_unread_ = std::atoi(f[1].c_str());
    else if (f[0] == "f") flight_prepaid_ = true;
  }
}

// ------------------------------------------------------------------------------------------------
// what happens between the apps: parcels and food arriving, the taxi ride / hotel night (travel)

void App::updatePhoneApps(float dt) {
  const int64_t now = clock_.unixUtc();
  for (auto it = orders_.begin(); it != orders_.end();) {
    if (now < it->due) {
      ++it;
      continue;
    }
    if (it->to_home) {
      // in the parcel box at the flat: collected when the player comes to the front door
      if (!home_.ok || !life_.has_home) {
        ++it;
        continue;
      }
      const double d = std::hypot((home_.door_a.x + home_.door_b.x) / 2 - player_.pos.x, (home_.door_a.y + home_.door_b.y) / 2 - player_.pos.y);
      if (d > 4.0) {
        if (!it->count) {
          ++it;
          continue;
        }
        if (it->count > 0) {  // (tell once that it has arrived: the count's sign marks it)
          message(tr("msg.from.parcel"), i18n_.f("msg.parcel_in_box", {{"item", tr("shop.item." + it->item)}}));
          it->count = -it->count;
        }
        ++it;
        continue;
      }
      inventory_[it->item] += std::abs(it->count);
      toast(i18n_.f("phone.shop.collected", {{"item", tr("shop.item." + it->item)}, {"n", std::to_string(std::abs(it->count))}}));
    } else {
      inventory_[it->item] += it->count;
      message(tr("msg.from.delivery"), i18n_.f("msg.food_arrived", {{"item", tr("shop.item." + it->item)}}));
    }
    it = orders_.erase(it);
  }
  // travel: stand still (not drawn) until the world around the destination has loaded
  if (travel_.on) {
    travel_.t += dt;
    player_.pos = world_.toLocal(travel_.dest);
    player_.vel_z = 0;
    const bool ready = world_.pendingJobs() == 0 && world_.terrainHeight(player_.pos.x, player_.pos.y).has_value();
    if (travel_.t > 0.8f && (ready || travel_.t > 20.0f)) {
      player_.pos.z = 1000.0;
      player_.snapToGround(world_);
      world_.collide(player_.pos, 0.35);
      travel_.on = false;
      if (!travel_.done.empty()) toast(travel_.done);
    }
  }
}

void App::startTravel(const rj::geo::Geodetic& g, const std::string& label, const std::string& done) {
  travel_.on = true;
  travel_.dest = g;
  travel_.t = 0;
  travel_.label = label;
  travel_.done = done;
  screen_ = Screen::Game;
}

void App::drawTravel() {
  if (!travel_.on) return;
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{6, 8, 12, 245});
  ui_.textCentered(travel_.label, ui_.vw() / 2, 500, 38, theme::kText);
  ui_.textCentered(tr("travel.note"), ui_.vw() / 2, 560, 22, theme::kMuted);
}

// ------------------------------------------------------------------------------------------------
// the apps

void App::drawPhoneMessages(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.messages"), cx, yy, 32, theme::kText);
  yy += 50;
  msgs_unread_ = 0;
  if (msgs_.empty()) {
    ui_.textWrapped(tr("msg.none"), cx, yy, cw, 22, theme::kMuted);
    return;
  }
  const float wheel = GetMouseWheelMove();
  phone_scroll_ = std::clamp(phone_scroll_ - wheel, 0.0f, static_cast<float>(std::max<int>(0, static_cast<int>(msgs_.size()) - 1)));
  for (int i = static_cast<int>(msgs_.size()) - 1 - static_cast<int>(phone_scroll_); i >= 0 && yy < bottom - 60; --i) {
    const auto& m = msgs_[static_cast<size_t>(i)];
    ui_.text(m.from, cx, yy, 22, theme::kWarn);
    ui_.textRight(stamp(m.t), cx + cw, yy + 2, 18, theme::kMuted);
    yy += 30;
    yy += ui_.textWrapped(m.text, cx, yy, cw, 20, theme::kText) + 14;
  }
}

void App::drawPhoneCall(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.call"), cx, yy, 32, theme::kText);
  yy += 50;
  if (!call_lines_.empty()) {
    ui_.text(call_title_, cx, yy, 24, theme::kWarn);
    yy += 40;
    for (const auto& l : call_lines_) yy += ui_.textWrapped(l, cx, yy, cw, 21, theme::kText) + 10;
    yy += 10;
    if (ui_.button({cx, yy, cw, 50}, tr("call.hang_up"), true, 22)) call_lines_.clear();
    ui_.textWrapped(tr("call.note"), cx, bottom - 50, cw, 18, theme::kMuted);
    return;
  }
  const auto t = jst();
  auto contact = [&](const char* key, const char* sub) {
    ui_.text(tr(key), cx, yy + 6, 24, theme::kText);
    ui_.text(tr(sub), cx, yy + 36, 16, theme::kMuted);
    const bool b = ui_.button({cx + cw - 110, yy + 8, 110, 44}, tr("call.dial"), true, 20);
    yy += 72;
    return b;
  };
  if (contact("call.weather", "call.weather_sub")) {
    // the forecast: the game's own weather run ahead on a copy (the same random sequence)
    call_title_ = tr("call.weather");
    call_lines_.clear();
    call_lines_.push_back(i18n_.f("call.weather_now", {{"w", tr(std::string("weather.") + weatherKey(weather_.kind()))}}));
    WeatherSim w = weather_;
    for (int h = 1; h <= 12; ++h) {
      w.update(3600.0, 3600.0, 30.0f);
      if (h == 3 || h == 6 || h == 12)
        call_lines_.push_back(i18n_.f("call.weather_at", {{"h", std::to_string(h)}, {"w", tr(std::string("weather.") + weatherKey(w.kind()))}}));
    }
    if (weather_.typhoonSeason()) call_lines_.push_back(tr("call.weather_typhoon_season"));
    if (weather_.tsuyu()) call_lines_.push_back(tr("call.weather_tsuyu"));
  }
  if (contact("call.time", "call.time_sub")) {
    call_title_ = tr("call.time");
    call_lines_ = {i18n_.f("call.time_line", {{"h", std::to_string(t.hour)}, {"m", std::to_string(t.minute)}, {"s", std::to_string(t.second)}})};
    audio_.cue(Cue::Beep, 0.6f);
  }
  if (contact("call.road", "call.road_sub")) {
    call_title_ = tr("call.road");
    call_lines_ = {tr(driving_.hasCar() ? "call.road_bag" : "call.road_nocar")};
  }
  if (contact("call.agent", "call.agent_sub")) phone_app_ = PhoneApp::Flat;
  if (contact("call.taxi", "call.taxi_sub")) phone_app_ = PhoneApp::Taxi;
}

void App::drawPhoneTransit(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.transit"), cx, yy, 32, theme::kText);
  yy += 50;
  const auto& st = trains_.stations();
  if (st.empty()) {
    ui_.textWrapped(tr("transit.none"), cx, yy, cw, 22, theme::kMuted);
    return;
  }
  int near = 0;
  double nd = 1e18;
  for (size_t i = 0; i < st.size(); ++i)
    if (const double d = std::hypot(st[i].pos.x - player_.pos.x, st[i].pos.y - player_.pos.y); d < nd) {
      nd = d;
      near = static_cast<int>(i);
    }
  const Station& S = st[static_cast<size_t>(near)];
  const auto& L = trains_.lines()[static_cast<size_t>(S.line)];
  const std::string lname = i18n_.code() == "ja" || L.name_en.empty() ? L.name : L.name_en;
  ui_.text(i18n_.f("transit.nearest", {{"st", S.name}}), cx, yy, 24, theme::kText);
  yy += 34;
  ui_.text(i18n_.f("transit.walk", {{"line", lname}, {"m", withCommas(static_cast<int64_t>(nd))}, {"min", std::to_string(static_cast<int>(nd / 80.0) + 1)}}), cx, yy,
           18, theme::kMuted);
  yy += 36;
  // the next trains to stop there: distance along the line to the station / a running speed, plus stops
  struct Next {
    double sec;
    std::string dest;
  };
  std::vector<Next> nx;
  for (const auto& t : trains_.trains()) {
    if (t.line != S.line) continue;
    double d = (S.s - t.s) * t.dir;
    if (d < -5.0) {
      if (!L.closed) continue;  // running away from it (it comes back after the terminus: not counted)
      d += L.length;
    }
    if (t.at_station == near) d = 0;
    const double run = std::max(8.0, t.vmax * 0.62);
    int stops = 0;
    for (const auto& o : st)
      if (o.line == S.line && &o != &S) {
        double od = (o.s - t.s) * t.dir;
        if (od < 0 && L.closed) od += L.length;
        if (od > 0 && od < d) ++stops;
      }
    std::string dest = trains_.destination(t);
    if (dest == "loop+") dest = tr("rail.dest.outer");
    else if (dest == "loop-") dest = tr("rail.dest.inner");
    nx.push_back({d / run + stops * 35.0 + (t.at_station >= 0 && t.at_station != near ? t.dwell : 0.0), dest});
  }
  std::sort(nx.begin(), nx.end(), [](const Next& a, const Next& b) { return a.sec < b.sec; });
  ui_.text(tr("transit.next"), cx, yy, 22, theme::kWarn);
  yy += 32;
  for (size_t i = 0; i < nx.size() && i < 4; ++i) {
    const int m = static_cast<int>(nx[i].sec / 60.0);
    ui_.text(i18n_.f("transit.train", {{"dest", nx[i].dest}}), cx + 10, yy, 20, theme::kText);
    ui_.textRight(m <= 0 ? tr("transit.now") : i18n_.f("transit.min", {{"m", std::to_string(m)}}), cx + cw, yy, 20, theme::kText);
    yy += 30;
  }
  yy += 8;
  ui_.textWrapped(tr("transit.eta_note"), cx, yy, cw, 16, theme::kMuted);
  yy += 44;
  // fare and time to another station
  ui_.text(tr("transit.to"), cx, yy, 22, theme::kWarn);
  yy += 32;
  if (transit_dest_ < 0 || transit_dest_ >= static_cast<int>(st.size())) transit_dest_ = (near + 1) % static_cast<int>(st.size());
  if (ui_.button({cx, yy, 50, 44}, "<", true, 24)) transit_dest_ = (transit_dest_ + static_cast<int>(st.size()) - 1) % static_cast<int>(st.size());
  if (ui_.button({cx + cw - 50, yy, 50, 44}, ">", true, 24)) transit_dest_ = (transit_dest_ + 1) % static_cast<int>(st.size());
  const Station& D = st[static_cast<size_t>(transit_dest_)];
  const auto& DL = trains_.lines()[static_cast<size_t>(D.line)];
  ui_.textCentered(D.name + "（" + (i18n_.code() == "ja" || DL.name_en.empty() ? DL.name : DL.name_en) + "）", cx + cw / 2, yy + 8, 20, theme::kText);
  yy += 56;
  if (D.line == S.line) {
    double km = std::fabs(D.s - S.s) / 1000.0;
    if (L.closed) km = std::min(km, L.length / 1000.0 - km);
    const bool shink = L.kind == LineKind::Shinkansen;
    const int64_t fare = railFare(shink, km);
    const int mins = static_cast<int>(km / (shink ? 3.2 : 0.9)) + 2;
    ui_.textWrapped(i18n_.f("transit.route", {{"km", fixed(km, 1)}, {"yen", withCommas(fare)}, {"min", std::to_string(mins)}}), cx, yy, cw, 20, theme::kText);
  } else {
    const double km = std::hypot(D.pos.x - S.pos.x, D.pos.y - S.pos.y) / 1000.0 * 1.2;
    ui_.textWrapped(i18n_.f("transit.route_change", {{"km", fixed(km, 0)}}), cx, yy, cw, 20, theme::kText);
  }
  (void)bottom;
}

void App::drawPhoneCamera(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.camera"), cx, yy, 32, theme::kText);
  yy += 56;
  ui_.textWrapped(i18n_.f("photo.desc", {{"n", std::to_string(photo_spots_.size())}, {"p", std::to_string(photos_taken_)}}), cx, yy, cw, 20, theme::kText);
  yy += 110;
  if (ui_.button({cx, yy, cw, 64}, tr("photo.open"), true, 26)) {
    photo_mode_ = true;
    photo_fov_ = settings_.fov;
    screen_ = Screen::Game;
  }
  yy += 84;
  ui_.textWrapped(tr("camera.where"), cx, yy, cw, 18, theme::kMuted);
  (void)bottom;
}

std::string App::trackTitle(int k) const {
  static const char* a[] = {"music.w.morning", "music.w.rain", "music.w.night", "music.w.slope", "music.w.harbour", "music.w.snow"};
  static const char* b[] = {"music.n.walk", "music.n.song", "music.n.waltz", "music.n.train", "music.n.window", "music.n.sky"};
  return tr(a[k % 6]) + tr(b[(k * 7 + 3) % 6]);
}

void App::drawPhoneMusic(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.music"), cx, yy, 32, theme::kText);
  yy += 52;
  for (int k = 0; k < Audio::musicTracks(); ++k) {
    const bool on = music_track_ == k;
    ui_.text(std::to_string(k + 1) + ". " + trackTitle(k), cx, yy + 10, 22, on ? theme::kGood : theme::kText);
    if (ui_.button({cx + cw - 110, yy + 4, 110, 44}, tr(on ? "music.stop" : "music.play"), true, 20)) music_track_ = on ? -1 : k;
    yy += 60;
  }
  yy += 10;
  ui_.textWrapped(tr("music.note"), cx, yy, cw, 18, theme::kMuted);
  (void)bottom;
}

void App::drawPhoneShopping(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.shopping"), cx, yy, 32, theme::kText);
  yy += 46;
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  const bool can = home_.ok && life_.has_home;
  ui_.textWrapped(tr(can ? "shopping.to_home" : "shopping.no_home"), cx, yy, cw, 18, can ? theme::kMuted : theme::kWarn);
  yy += 50;
  for (const auto& c : kShop) {
    const int64_t price = c.yen + kShipping;
    std::string name = tr(std::string("shop.item.") + c.key);
    if (c.count > 1) name += " ×" + std::to_string(c.count);
    ui_.text(name, cx, yy + 8, 20, theme::kText);
    if (ui_.button({cx + cw - 150, yy, 150, 40}, "¥" + withCommas(price), can && bal >= price, 19) && ledger_ &&
        ledger_->transfer(player_account_, ledger_->externalAccount(), price, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("phone.shopping")) ==
            rj::econ::TxResult::Ok) {
      // tomorrow: at 10:00 if ordered by 18:00, else at 14:00
      const auto t = jst();
      const int64_t midnight = rj::sim::GameClock::unixFromJst(t.date, 0, 0);
      const int64_t due = midnight + 86400 + (t.hour < 18 ? 10 : 14) * 3600;
      orders_.push_back({due, c.key, c.count, true});
      toast(i18n_.f("phone.shop.ordered", {{"when", stamp(due)}}));
    }
    yy += 46;
  }
  yy += 6;
  for (const auto& o : orders_)
    if (o.to_home && yy < bottom - 30) {
      ui_.text(tr("shop.item." + o.item) + "  " + (o.count < 0 ? tr("shopping.in_box") : i18n_.f("shopping.due", {{"when", stamp(o.due)}})), cx, yy, 17,
               theme::kMuted);
      yy += 24;
    }
}

void App::drawPhoneDelivery(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.delivery"), cx, yy, 32, theme::kText);
  yy += 46;
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  const bool ok = !driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0 && !flying_;
  ui_.textWrapped(tr(ok ? "delivery.here" : "delivery.not_now"), cx, yy, cw, 18, ok ? theme::kMuted : theme::kWarn);
  yy += 50;
  for (const auto& c : kFood) {
    const int64_t price = c.yen + kDeliveryFee;
    ui_.text(tr(std::string("shop.item.") + c.key), cx, yy + 8, 22, theme::kText);
    if (ui_.button({cx + cw - 150, yy, 150, 44}, "¥" + withCommas(price), ok && bal >= price, 20) && ledger_ &&
        ledger_->transfer(player_account_, ledger_->externalAccount(), price, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("phone.delivery")) ==
            rj::econ::TxResult::Ok) {
      const int mins = 25 + static_cast<int>(mix(static_cast<uint32_t>(clock_.unixUtc()), 5) % 16);
      orders_.push_back({clock_.unixUtc() + mins * 60, c.key, 1, false});
      toast(i18n_.f("delivery.ordered", {{"min", std::to_string(mins)}}));
    }
    yy += 56;
  }
  yy += 6;
  for (const auto& o : orders_)
    if (!o.to_home && yy < bottom - 30) {
      ui_.text(tr("shop.item." + o.item) + "  " + i18n_.f("shopping.due", {{"when", stamp(o.due)}}), cx, yy, 17, theme::kMuted);
      yy += 24;
    }
  ui_.textWrapped(tr("delivery.note"), cx, bottom - 44, cw, 16, theme::kMuted);
}

void App::drawPhoneTaxi(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.taxi"), cx, yy, 32, theme::kText);
  yy += 50;
  // destinations: the flat, the stations, the towns
  struct Dest {
    std::string name;
    rj::geo::Geodetic g;
  };
  std::vector<Dest> ds;
  if (home_.ok) {
    const double ox = (home_.door_a.x + home_.door_b.x) / 2 - home_.bed.x, oy = (home_.door_a.y + home_.door_b.y) / 2 - home_.bed.y, ol = std::max(1e-6, std::hypot(ox, oy));
    ds.push_back({tr("home.map_mine"), world_.toGeodetic({home_.bed.x + ox + ox / ol * 2.5, home_.bed.y + oy + oy / ol * 2.5, home_.bed.z})});
  }
  for (const auto& s : trains_.stations()) {
    const double th = (s.heading + 90.0) * DEG2RAD;
    ds.push_back({s.name + tr("taxi.station"), world_.toGeodetic({s.pos.x + std::sin(th) * 22.0, s.pos.y + std::cos(th) * 22.0, s.pos.z})});
  }
  for (const auto& p : world_.meta().places) ds.push_back({i18n_.code() == "ja" || p.name_en.empty() ? p.name : p.name_en, {p.lat, p.lon, 0.0}});
  if (ds.empty()) {
    ui_.textWrapped(tr("taxi.none"), cx, yy, cw, 22, theme::kMuted);
    return;
  }
  if (taxi_dest_ < 0 || taxi_dest_ >= static_cast<int>(ds.size())) taxi_dest_ = 0;
  if (ui_.button({cx, yy, 50, 48}, "<", true, 24)) taxi_dest_ = (taxi_dest_ + static_cast<int>(ds.size()) - 1) % static_cast<int>(ds.size());
  if (ui_.button({cx + cw - 50, yy, 50, 48}, ">", true, 24)) taxi_dest_ = (taxi_dest_ + 1) % static_cast<int>(ds.size());
  const Dest& D = ds[static_cast<size_t>(taxi_dest_)];
  ui_.textCentered(D.name, cx + cw / 2, yy + 10, 22, theme::kText);
  yy += 70;
  const rj::geo::Vec3d q = world_.toLocal(D.g);
  const double straight = std::hypot(q.x - player_.pos.x, q.y - player_.pos.y), road = straight * 1.3;
  const auto t = jst();
  const int64_t fare = taxiFare(road, t.hour);
  const int mins = static_cast<int>(road / 1000.0 / (road > 8000 ? 45.0 : 25.0) * 60.0) + 4;
  ui_.text(i18n_.f("taxi.estimate", {{"km", fixed(road / 1000.0, 1)}, {"min", std::to_string(mins)}}), cx, yy, 20, theme::kText);
  yy += 34;
  ui_.text(i18n_.f("taxi.fare", {{"yen", withCommas(fare)}}), cx, yy, 26, theme::kWarn);
  yy += 50;
  const bool ok = !driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0 && !flying_ && !player_.fly && inside_id_.empty() && straight > 150.0;
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  if (ui_.button({cx, yy, cw, 60}, tr("taxi.go"), ok && bal >= fare, 24) && ledger_ &&
      ledger_->transfer(player_account_, ledger_->externalAccount(), fare, rj::econ::TxCategory::Fare, clock_.unixUtc(), tr("phone.taxi")) ==
          rj::econ::TxResult::Ok) {
    clock_.advanceGame(mins * 60.0 + 300.0);  // (the car comes in about five minutes)
    message(tr("msg.from.taxi"), i18n_.f("msg.taxi_receipt", {{"dest", D.name}, {"yen", withCommas(fare)}}));
    startTravel(D.g, i18n_.f("taxi.riding", {{"dest", D.name}}), i18n_.f("taxi.arrived", {{"dest", D.name}}));
  }
  yy += 76;
  ui_.textWrapped(tr("taxi.note"), cx, yy, cw, 17, theme::kMuted);
  (void)bottom;
}

void App::drawPhoneFlights(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.flights"), cx, yy, 32, theme::kText);
  yy += 50;
  if (!aviation_.loaded() || aviation_.airports().size() < 2) {
    ui_.textWrapped(tr("flights.none"), cx, yy, cw, 22, theme::kMuted);
    return;
  }
  for (const auto& a : aviation_.airliners()) {
    std::string state;
    using P = Airliner::Phase;
    switch (a.phase) {
      case P::AtStand: state = i18n_.f("flights.at_stand", {{"ap", airportName(a.from)}, {"min", std::to_string(std::max(1, static_cast<int>(a.timer / 60.0)))}}); break;
      case P::Pushback:
      case P::TaxiOut:
      case P::Takeoff: state = i18n_.f("flights.departing", {{"ap", airportName(a.from)}}); break;
      case P::Climb:
      case P::Offmap:
      case P::Approach: state = i18n_.f("flights.en_route", {{"ap", airportName(a.to)}}); break;
      case P::Landing:
      case P::TaxiIn: state = i18n_.f("flights.arriving", {{"ap", airportName(a.to)}}); break;
    }
    ui_.text(i18n_.f("flights.flight", {{"n", std::to_string(101 + a.id)}, {"from", airportName(a.from)}, {"to", airportName(a.to)}}), cx, yy, 21, theme::kText);
    yy += 28;
    ui_.text(state, cx + 10, yy, 18, theme::kMuted);
    yy += 36;
  }
  yy += 10;
  ui_.text(tr("flights.fare"), cx, yy, 22, theme::kText);
  yy += 40;
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  if (flight_prepaid_) {
    ui_.textWrapped(tr("flights.have_ticket"), cx, yy, cw, 20, theme::kGood);
  } else if (ui_.button({cx, yy, cw, 56}, tr("flights.buy"), bal >= 12800, 22) && ledger_ &&
             ledger_->transfer(player_account_, ledger_->externalAccount(), 12800, rj::econ::TxCategory::Fare, clock_.unixUtc(), tr("jet.airline")) ==
                 rj::econ::TxResult::Ok) {
    flight_prepaid_ = true;
    message(tr("jet.airline"), tr("msg.flight_ticket"));
  }
  yy += 76;
  ui_.textWrapped(tr("flights.note"), cx, yy, cw, 17, theme::kMuted);
  (void)bottom;
}

void App::drawPhoneHotel(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.hotel"), cx, yy, 32, theme::kText);
  yy += 50;
  int near = -1;
  double nd = 700.0;
  for (size_t i = 0; i < trains_.stations().size(); ++i) {
    const auto& s = trains_.stations()[i];
    if (const double d = std::hypot(s.pos.x - player_.pos.x, s.pos.y - player_.pos.y); d < nd) {
      nd = d;
      near = static_cast<int>(i);
    }
  }
  if (near < 0) {
    ui_.textWrapped(tr("hotel.far"), cx, yy, cw, 21, theme::kMuted);
    return;
  }
  ui_.text(i18n_.f("hotel.name", {{"st", trains_.stations()[static_cast<size_t>(near)].name}}), cx, yy, 24, theme::kText);
  yy += 38;
  ui_.text(i18n_.f("hotel.price", {{"yen", withCommas(kHotelYen)}}), cx, yy, 22, theme::kText);
  yy += 44;
  const int64_t bal = ledger_ ? ledger_->balance(player_account_) : 0;
  const bool ok = !driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0 && !flying_ && !player_.fly;
  if (ui_.button({cx, yy, cw, 60}, tr("hotel.stay"), ok && bal >= kHotelYen, 24) && ledger_ &&
      ledger_->transfer(player_account_, ledger_->externalAccount(), kHotelYen, rj::econ::TxCategory::Purchase, clock_.unixUtc(), tr("phone.hotel")) ==
          rj::econ::TxResult::Ok) {
    const auto g = world_.toGeodetic(player_.pos);
    sleepAtHome();  // (the same night's sleep: to 7:00, or a two-hour rest by day)
    startTravel(g, tr("hotel.sleeping"), tr("hotel.morning"));
  }
  yy += 80;
  ui_.textWrapped(tr("hotel.note"), cx, yy, cw, 17, theme::kMuted);
  (void)bottom;
}

void App::drawPhoneSns(float cx, float yy, float cw, float bottom) {
  ui_.text(tr("phone.sns"), cx, yy, 32, theme::kText);
  yy += 44;
  ui_.textWrapped(tr("sns.note"), cx, yy, cw, 16, theme::kMuted);
  yy += 44;
  const auto t = jst();
  const int64_t bucket = clock_.unixUtc() / 1200;  // (a fresh page every 20 game minutes)
  const int n_people = static_cast<int>(town_.size());
  const WeatherKind wk = weather_.kind();
  const bool rain = weather_.now().rain_mm_h > 0.3f;
  const float wheel = GetMouseWheelMove();
  phone_scroll_ = std::clamp(phone_scroll_ - wheel, 0.0f, 10.0f);
  for (int k = static_cast<int>(phone_scroll_); k < 14 && yy < bottom - 70; ++k) {
    const uint32_t h = mix(static_cast<uint32_t>(bucket), static_cast<uint32_t>(k));
    std::string who, text;
    if (n_people > 0) {
      const auto& p = town_.npc(static_cast<size_t>(h % static_cast<uint32_t>(n_people)));
      who = p.given_name + "_" + std::to_string(10 + h % 90);
      const int act = static_cast<int>(town_.activityOf(static_cast<size_t>(h % static_cast<uint32_t>(n_people)), t.date, t.minuteOfDay()));
      const int kind = static_cast<int>((h >> 8) % 5);
      if (kind == 0) text = tr("sns.act." + std::to_string(act));
      else if (kind == 1) text = tr(wk == WeatherKind::Typhoon ? "sns.typhoon" : rain ? "sns.rain" : (t.date.m == 4 && t.date.d <= 12) ? "sns.sakura" : weather_.tsuyu() ? "sns.tsuyu" : snowfall_ > 0.2f ? "sns.snow" : t.hour >= 18 || t.hour < 5 ? "sns.night" : "sns.fine");
      else if (kind == 2 && !world_.meta().pois.empty()) text = i18n_.f("sns.place", {{"place", world_.meta().pois[(h >> 12) % world_.meta().pois.size()].name}});
      else if (kind == 3 && trains_.loaded()) text = tr("sns.train_" + std::to_string((h >> 14) % 3));
      else text = tr("sns.misc_" + std::to_string((h >> 16) % 6));
    } else {
      who = "akitsu_" + std::to_string(h % 100);
      text = tr("sns.misc_" + std::to_string((h >> 16) % 6));
    }
    const int ago = static_cast<int>((h >> 20) % 19) + 1;
    const int likes = static_cast<int>((h >> 4) % 40) + (sns_liked_.count(static_cast<int>(h)) ? 1 : 0);
    ui_.text("@" + who, cx, yy, 19, theme::kAccent);
    ui_.textRight(i18n_.f("sns.ago", {{"m", std::to_string(ago)}}), cx + cw, yy, 16, theme::kMuted);
    yy += 26;
    yy += ui_.textWrapped(text, cx, yy, cw - 90, 19, theme::kText);
    const bool liked = sns_liked_.count(static_cast<int>(h)) > 0;
    if (ui_.button({cx + cw - 96, yy - 30, 96, 32}, tr(liked ? "sns.liked" : "sns.like") + " " + std::to_string(likes), true, 15)) {
      if (liked) sns_liked_.erase(static_cast<int>(h));
      else sns_liked_.insert(static_cast<int>(h));
    }
    yy += 16;
  }
}

// ------------------------------------------------------------------------------------------------
// captions: the station and on-board announcements, the cabin crew, the tower (fixed text, no voices)

void App::drawCaption(float dt) {
  if (caption_t_ <= 0.0f || caption_.empty() || !ui_.hasFont()) return;
  caption_t_ -= dt;
  if (screen_ != Screen::Game) return;
  const float a = std::clamp(caption_t_, 0.0f, 1.0f);
  const float w = std::min(1300.0f, ui_.measure(caption_, 24) + 60.0f);
  ui_.panel({(ui_.vw() - w) / 2, 860, w, 64}, Color{0, 0, 0, static_cast<unsigned char>(170 * a)});
  ui_.textCentered(caption_, ui_.vw() / 2, 870, 24, Color{255, 250, 220, static_cast<unsigned char>(255 * a)});
  ui_.textCentered(tr("pa.note"), ui_.vw() / 2, 900, 15, Color{200, 200, 200, static_cast<unsigned char>(200 * a)});
}

void App::updateAtc() {
  // the tower's calls to the light aircraft (a fixed script by the flight's stages)
  if (!aviation_.loaded()) return;
  const LightPlane& pl = aviation_.plane();
  int ap = 0;
  double ad = 1e18;
  for (size_t i = 0; i < aviation_.airports().size(); ++i) {
    const auto& A = aviation_.airports()[i];
    if (!A.ok) continue;
    const double d = std::hypot((A.rwy_a.x + A.rwy_b.x) / 2 - pl.pos().x, (A.rwy_a.y + A.rwy_b.y) / 2 - pl.pos().y);
    if (d < ad) {
      ad = d;
      ap = static_cast<int>(i);
    }
  }
  const auto& A = aviation_.airports()[static_cast<size_t>(ap)];
  double hd = std::atan2(A.rwy_b.x - A.rwy_a.x, A.rwy_b.y - A.rwy_a.y) * RAD2DEG;
  if (hd < 0) hd += 360.0;
  const int rwy = std::max(1, static_cast<int>(std::lround(hd / 10.0)) % 36);
  const std::string tower = airportName(ap), rw = (rwy < 10 ? "0" : "") + std::to_string(rwy);
  const std::string wind = std::to_string(static_cast<int>(std::lround(weather_.now().wind_ms * 1.944)));
  const double agl = pl.pos().z - A.rwy_a.z;
  if (atc_stage_ < 0) {
    atc_stage_ = 0;
    caption(i18n_.f("atc.taxi", {{"tower", tower}, {"rwy", rw}}));
  } else if (atc_stage_ == 0 && pl.onGround() && pl.airspeed() > 10.0) {
    atc_stage_ = 1;
    caption(i18n_.f("atc.takeoff", {{"tower", tower}, {"rwy", rw}, {"wind", wind}}));
  } else if (atc_stage_ == 1 && !pl.onGround() && agl > 60.0) {
    atc_stage_ = 2;
    caption(i18n_.f("atc.airborne", {{"tower", tower}}));
  } else if (atc_stage_ == 2 && ad < 4000.0 && agl < 400.0 && pl.verticalSpeed() < -0.5) {
    atc_stage_ = 3;
    caption(i18n_.f("atc.land", {{"tower", tower}, {"rwy", rw}, {"wind", wind}}));
  } else if (atc_stage_ >= 2 && pl.onGround() && pl.airspeed() < 15.0) {
    atc_stage_ = 0;
    caption(i18n_.f("atc.landed", {{"tower", tower}}));
  }
}

}  // namespace rjc
