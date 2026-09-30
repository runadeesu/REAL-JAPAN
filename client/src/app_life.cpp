// The player's body and belongings: hunger and thirst (they fall with the game hours; at zero the
// player cannot run), rain soaking the clothes without an umbrella, and using what was bought in
// the shops - eating, drinking, the umbrella, the flashlight and its batteries, the notebook, the
// towel. The phone's "bag" app shows it all. Every number here is a game value.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app.hpp"
#include "ui/ui.hpp"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {

enum class UseKind { Eat, Drink, Umbrella, Light, Batteries, Note, Towel, Brush };
struct ItemUse {
  const char* key;
  float food, drink;
  UseKind kind;
  bool consumed;
};
const ItemUse kUses[] = {
    {"onigiri", 30, 0, UseKind::Eat, true},     {"sandwich", 35, 5, UseKind::Eat, true},     {"bento", 60, 5, UseKind::Eat, true},
    {"green_tea", 0, 40, UseKind::Drink, true}, {"coffee_can", 0, 25, UseKind::Drink, true}, {"ice_cream", 10, 10, UseKind::Eat, true},
    {"blend", 0, 30, UseKind::Drink, true},     {"latte", 5, 35, UseKind::Drink, true},      {"tea", 0, 30, UseKind::Drink, true},
    {"cake", 25, 5, UseKind::Eat, true},        {"toast", 35, 0, UseKind::Eat, true},        {"umbrella", 0, 0, UseKind::Umbrella, false},
    {"flashlight", 0, 0, UseKind::Light, false}, {"batteries", 0, 0, UseKind::Batteries, true}, {"notebook", 0, 0, UseKind::Note, false},
    {"towel", 0, 0, UseKind::Towel, false},     {"toothbrush", 0, 0, UseKind::Brush, false},
};

const ItemUse* findUse(const std::string& k) {
  for (const auto& u : kUses)
    if (k == u.key) return &u;
  return nullptr;
}

}  // namespace

std::string App::lifeString() const {
  char b[320];
  std::snprintf(b, sizeof b, "hunger:%.1f;thirst:%.1f;wet:%.2f;umbrella:%d;light:%d;battery:%.3f;fuel:%.3f;damage:%.3f;home:%d;rent:%lld;talks:%d",
                life_.hunger, life_.thirst, life_.wet, life_.umbrella ? 1 : 0, life_.flashlight ? 1 : 0, life_.battery, life_.fuel, life_.damage,
                life_.has_home ? 1 : 0, static_cast<long long>(life_.rent_paid_until), life_.talks);
  return b;
}

void App::parseLife(const std::string& s) {
  life_ = Life{};
  std::istringstream in(s);
  std::string part;
  while (std::getline(in, part, ';')) {
    const auto c = part.find(':');
    if (c == std::string::npos) continue;
    const std::string k = part.substr(0, c);
    const double v = std::atof(part.c_str() + c + 1);
    if (k == "hunger") life_.hunger = static_cast<float>(std::clamp(v, 0.0, 100.0));
    else if (k == "thirst") life_.thirst = static_cast<float>(std::clamp(v, 0.0, 100.0));
    else if (k == "wet") life_.wet = static_cast<float>(std::clamp(v, 0.0, 1.0));
    else if (k == "umbrella") life_.umbrella = v > 0.5;
    else if (k == "light") life_.flashlight = v > 0.5;
    else if (k == "battery") life_.battery = static_cast<float>(std::clamp(v, 0.0, 1.0));
    else if (k == "fuel") life_.fuel = static_cast<float>(std::clamp(v, 0.0, 1.0));
    else if (k == "damage") life_.damage = static_cast<float>(std::clamp(v, 0.0, 1.0));
    else if (k == "home") life_.has_home = v > 0.5;
    else if (k == "rent") life_.rent_paid_until = static_cast<int64_t>(v);
    else if (k == "talks") life_.talks = static_cast<int>(v);
  }
}

std::string App::notesString() const {
  std::string s;
  for (const auto& n : notes_) {
    if (!s.empty()) s += '|';
    std::string clean = n;
    std::replace(clean.begin(), clean.end(), '|', '/');
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    s += clean;
  }
  return s;
}

void App::parseNotes(const std::string& s) {
  notes_.clear();
  std::istringstream in(s);
  std::string part;
  while (std::getline(in, part, '|'))
    if (!part.empty()) notes_.push_back(part);
}

void App::updateLife(float dt) {
  // the game hours gone by (also across a night's sleep or a quick time skip)
  const int64_t now = clock_.unixUtc();
  if (life_.last_unix == 0 || now < life_.last_unix || now - life_.last_unix > 3 * 86400) life_.last_unix = now;
  const double h = static_cast<double>(now - life_.last_unix) / 3600.0;
  if (h > 0.0) {
    life_.last_unix = now;
    const float h0 = life_.hunger, t0 = life_.thirst;
    life_.hunger = std::max(0.0f, life_.hunger - static_cast<float>(6.0 * h));  // a full stomach lasts about 16 hours
    life_.thirst = std::max(0.0f, life_.thirst - static_cast<float>(9.0 * h));  // a drink about 11
    if (h0 >= 25.0f && life_.hunger < 25.0f) toast(tr("life.hungry"));
    if (t0 >= 25.0f && life_.thirst < 25.0f) toast(tr("life.thirsty"));
    if ((h0 > 0.0f && life_.hunger <= 0.0f) || (t0 > 0.0f && life_.thirst <= 0.0f)) toast(tr("life.cannot_run"));
    if (life_.flashlight) {
      life_.battery = std::max(0.0f, life_.battery - static_cast<float>(h / 8.0));  // a set of batteries: about 8 hours
      if (life_.battery <= 0.0f) {
        life_.flashlight = false;
        toast(tr("life.battery_empty"));
      }
    }
  }
  player_.can_run = life_.hunger > 0.0f && life_.thirst > 0.0f;
  // rain soaks the clothes out of doors without an open umbrella; they dry slowly
  const bool outdoors = in_session_ && inside_id_.empty() && lighting_.indoor < 0.5f && !driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 &&
                        ride_jet_ < 0 && !flying_;
  const float rain = weather_.now().rain_mm_h;
  const float w0 = life_.wet;
  if (outdoors && rain > 0.3f && !life_.umbrella) life_.wet = std::min(1.0f, life_.wet + dt * std::min(1.0f, rain / 8.0f) * 0.02f);
  else life_.wet = std::max(0.0f, life_.wet - dt * (outdoors ? 0.0015f : 0.004f));
  if (w0 < 0.6f && life_.wet >= 0.6f) toast(tr("life.soaked"));
}

bool App::canUseItem(const std::string& key) const {
  const auto it = inventory_.find(key);
  return it != inventory_.end() && it->second > 0 && findUse(key);
}

bool App::useItem(const std::string& key) {
  const ItemUse* u = findUse(key);
  auto it = inventory_.find(key);
  if (!u || it == inventory_.end() || it->second <= 0) return false;
  const std::string name = tr("shop.item." + key);
  switch (u->kind) {
    case UseKind::Eat:
    case UseKind::Drink:
      life_.hunger = std::min(100.0f, life_.hunger + u->food);
      life_.thirst = std::min(100.0f, life_.thirst + u->drink);
      toast(i18n_.f(u->kind == UseKind::Eat ? "life.ate" : "life.drank", {{"item", name}}));
      break;
    case UseKind::Umbrella:
      life_.umbrella = !life_.umbrella;
      toast(tr(life_.umbrella ? "life.umbrella_open" : "life.umbrella_closed"));
      break;
    case UseKind::Light:
      if (!life_.flashlight && life_.battery <= 0.0f) {
        toast(tr("life.battery_empty"));
        return false;
      }
      life_.flashlight = !life_.flashlight;
      toast(tr(life_.flashlight ? "life.light_on" : "life.light_off"));
      break;
    case UseKind::Batteries:
      life_.battery = 1.0f;
      toast(tr("life.batteries"));
      break;
    case UseKind::Note: {
      // the date, the time and where: the nearest named place within 1.5 km
      const auto t = jst();
      std::string place;
      double best = 1500.0;
      for (const auto& p : world_.meta().pois) {
        const rj::geo::Vec3d q = world_.toLocal({p.lat, p.lon, 0.0});
        if (const double d = std::hypot(q.x - player_.pos.x, q.y - player_.pos.y); d < best) {
          best = d;
          place = p.name;
        }
      }
      if (place.empty()) place = tr("life.note_nowhere");
      char b[64];
      std::snprintf(b, sizeof b, "%d/%d %02d:%02d ", t.date.m, t.date.d, t.hour, t.minute);
      notes_.push_back(std::string(b) + place);
      if (notes_.size() > 40) notes_.erase(notes_.begin());
      toast(i18n_.f("life.noted", {{"place", place}}));
      break;
    }
    case UseKind::Towel:
      life_.wet = 0.0f;
      toast(tr("life.towel"));
      break;
    case UseKind::Brush:
      toast(tr("life.brushed"));
      break;
  }
  if (u->consumed && --it->second <= 0) inventory_.erase(it);
  return true;
}

void App::drawBag(float x, float y, float w) {
  // the body, then everything carried with a button to use it
  float yy = y;
  auto bar = [&](const std::string& label, float v, Color c) {
    ui_.text(label, x, yy, 24, theme::kText);
    DrawRectangleRec(ui_.px({x + 150, yy + 6, w - 150, 18}), Color{50, 50, 56, 255});
    DrawRectangleRec(ui_.px({x + 150, yy + 6, (w - 150) * std::clamp(v / 100.0f, 0.0f, 1.0f), 18}), v < 25.0f ? theme::kWarn : c);
    yy += 36;
  };
  bar(tr("life.hunger"), life_.hunger, theme::kGood);
  bar(tr("life.thirst"), life_.thirst, Color{80, 150, 220, 255});
  if (life_.wet > 0.05f) bar(tr("life.wet"), life_.wet * 100.0f, Color{120, 160, 200, 255});
  std::string state;
  if (life_.umbrella) state += tr("life.state_umbrella") + "  ";
  if (life_.flashlight) state += i18n_.f("life.state_light", {{"n", std::to_string(static_cast<int>(life_.battery * 100.0f))}}) + "  ";
  if (!state.empty()) {
    ui_.text(state, x, yy, 22, theme::kMuted);
    yy += 32;
  }
  drawCarPhone(x, yy, w);
  yy += 6;
  ui_.text(tr("life.items"), x, yy, 26, theme::kMuted);
  yy += 40;
  if (inventory_.empty()) {
    ui_.text(tr("phone.wallet_items_none"), x, yy, 22, theme::kText);
    yy += 36;
  }
  std::string used;
  int shown = 0;
  for (const auto& [k, n] : inventory_) {
    if (n <= 0 || shown >= 9) continue;
    ++shown;
    ui_.text(tr("shop.item." + k) + " ×" + std::to_string(n), x, yy + 10, 24, theme::kText);
    const ItemUse* u = findUse(k);
    if (u) {
      const char* verb = u->kind == UseKind::Eat ? "life.verb_eat" : u->kind == UseKind::Drink ? "life.verb_drink" : "life.verb_use";
      if (ui_.button({x + w - 150, yy, 150, 46}, tr(verb), true, 22.0f)) used = k;
    }
    yy += 54;
  }
  if (!used.empty()) useItem(used);
  if (!notes_.empty()) {
    yy += 6;
    ui_.text(tr("life.notes"), x, yy, 24, theme::kMuted);
    yy += 34;
    for (size_t i = notes_.size() > 3 ? notes_.size() - 3 : 0; i < notes_.size(); ++i) {
      ui_.text(notes_[i], x, yy, 20, theme::kText);
      yy += 28;
    }
  }
}

void App::drawLifeHud() {
  // (immersive view: a quiet line at the top left only when something needs attention)
  std::string s;
  if (life_.hunger <= 0.0f || life_.thirst <= 0.0f) s = tr("life.cannot_run");
  else if (life_.hunger < 25.0f) s = tr("life.hungry");
  else if (life_.thirst < 25.0f) s = tr("life.thirsty");
  if (life_.wet >= 0.6f) s += (s.empty() ? "" : "  ·  ") + tr("life.soaked");
  if (s.empty()) return;
  const float w = ui_.measure(s, 22) + 30;
  ui_.panel({24, 24, w, 40}, Color{0, 0, 0, 120});
  ui_.text(s, 39, 32, 22, theme::kWarn);
}

}  // namespace rjc
