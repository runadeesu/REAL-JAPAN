// Talking to people in the street: look at someone close by and press E (X on a controller, a tap)
// and they stop, turn to you and say a few words - a greeting for the time of day, who they are
// and where they are off to (from their resident record and today's schedule), the weather or the
// season, a word about the place. The lines are fixed templates filled in from the game's data,
// not a language model, and there are no voices. People you have talked to remember you (for the
// session).

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app.hpp"
#include "rj/sim/npc.hpp"
#include "ui/ui.hpp"
#include "world/coords.hpp"

namespace rjc {

void App::updateTalk(float dt) {
  if (talk_t_ > 0.0f) {
    talk_t_ -= dt;
    const bool gone = std::hypot(talk_at_.x - player_.pos.x, talk_at_.y - player_.pos.y) > 6.0;
    if (gone || talk_t_ <= 0.0f || (talk_t_ < 8.5f && IsKeyPressed(KEY_E))) talk_t_ = 0.0f;
    return;
  }
  static bool test = std::getenv("RJ_TALK_TEST") != nullptr;  // test aid: talk to the nearest resident once
  if (test) {
    const Walker* best = nullptr;
    double bd = 150.0;
    for (const auto& [id, w] : peds_.walkers())
      if (const double d = std::hypot(w.pos.x - player_.pos.x, w.pos.y - player_.pos.y); d < bd) {
        bd = d;
        best = &w;
      }
    if (best) {
      startTalk(*best);
      talk_t_ = 1e6f;  // (stays up for the screenshot)
      test = false;
    }
    return;
  }
  if (!hover_walker_ || aim_icon_ != AimIcon::None || driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_) return;
  const Walker& w = *hover_walker_;
  if (std::hypot(w.pos.x - player_.pos.x, w.pos.y - player_.pos.y) > 3.2) return;
  aim_icon_ = AimIcon::Hand;
  aim_label_ = tr("aim.talk");
  if (usePressed()) startTalk(w);
}

void App::startTalk(const Walker& w) {
  const size_t id = peds_.idOf(&w);
  const int met = talked_[id]++;
  ++life_.talks;
  talk_lines_.clear();
  const auto t = jst();
  const uint32_t h = static_cast<uint32_t>(id * 2654435761u) ^ static_cast<uint32_t>(life_.talks * 40503u);
  // greeting
  talk_lines_.push_back(tr(met > 0 ? "talk.again" : t.hour < 10 ? "talk.greet_morning" : t.hour < 17 ? "talk.greet_day" : "talk.greet_evening"));
  if (w.visitor) {
    talk_speaker_ = tr("talk.visitor");
    talk_lines_.push_back(tr((h & 1) ? "talk.visitor_1" : "talk.visitor_2"));
  } else {
    const auto& n = town_.npc(w.npc);
    const auto* occ = rj::sim::occupationById(n.occupation_id);
    const std::string job = occ ? std::string(i18n_.code() == "ja" ? occ->name_ja : occ->name_en) : n.occupation_id;
    talk_speaker_ = n.fullName();
    if (met == 0) talk_lines_.push_back(i18n_.f("talk.self", {{"name", n.fullName()}, {"job", job}, {"age", std::to_string(n.age)}}));
    talk_lines_.push_back(i18n_.f("talk.going", {{"next", tr("activity." + std::to_string(static_cast<int>(w.trip.next)))}}));
  }
  // the weather or the season
  const WeatherKind k = weather_.kind();
  std::string sky;
  if (k == WeatherKind::Typhoon) sky = "talk.typhoon";
  else if (weather_.now().rain_mm_h > 0.3f) sky = life_.umbrella ? "talk.rain" : "talk.rain_no_umbrella";
  else if (t.date.m == 4 && t.date.d <= 12) sky = "talk.sakura";
  else if (weather_.tsuyu()) sky = "talk.tsuyu";
  else if (snowfall_ > 0.2f) sky = "talk.snow";
  else if (k == WeatherKind::Clear || k == WeatherKind::Fair) sky = t.hour >= 18 || t.hour < 5 ? "talk.night" : "talk.fine";
  else sky = "talk.cloudy";
  talk_lines_.push_back(tr(sky));
  // a word about somewhere near (the nearest named place), or a tip
  std::string place;
  double best = 2500.0;
  for (const auto& p : world_.meta().pois) {
    const rj::geo::Vec3d q = world_.toLocal({p.lat, p.lon, 0.0});
    if (const double d = std::hypot(q.x - player_.pos.x, q.y - player_.pos.y); d < best && d > 40.0) {
      best = d;
      place = p.name;
    }
  }
  if (!place.empty() && (h >> 3) % 3 != 0) talk_lines_.push_back(i18n_.f("talk.place", {{"place", place}}));
  else talk_lines_.push_back(tr("talk.tip_" + std::to_string((h >> 5) % 5)));
  talk_t_ = 9.0f;
  talk_at_ = player_.pos;
  peds_.hold(id, 9.0f, player_.pos);
}

void App::drawTalk() {
  if (talk_t_ <= 0.0f || talk_lines_.empty()) return;
  const float vw = ui_.vw(), w = std::min(1100.0f, vw - 80), x = (vw - w) / 2;
  const float h = 70 + 36 * static_cast<float>(talk_lines_.size()), y = 1080 - 170 - h;
  ui_.panel({x, y, w, h}, Color{10, 12, 18, 215});
  ui_.text(talk_speaker_, x + 26, y + 16, 26, theme::kWarn);
  float yy = y + 56;
  for (const auto& l : talk_lines_) {
    ui_.text(l, x + 26, yy, 26, theme::kText);
    yy += 36;
  }
  ui_.textRight(tr("talk.note"), x + w - 20, y + 20, 18, theme::kMuted);
}

}  // namespace rjc
