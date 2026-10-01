// The guitar, the band and busking. A guitar bought at a general shop is taken out from the bag;
// keys 1-8 (on a controller the face buttons, shoulders and triggers; on touch the note buttons)
// play the notes of a major pentatonic scale as plucked strings (synthesised, audio.cpp). N calls
// the band: up to two residents who play (the band / instrument hobby) stand beside you and the
// rhythm section (bass, soft drums, chords) plays a backing in its own key and tempo, and the
// guitar follows that key. Passers-by who like what they hear stop to listen, and some leave a tip
// (game values). Playing in a steady rhythm draws more people; so does the square by a station.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app.hpp"
#include "rj/sim/npc.hpp"
#include "ui/ui.hpp"
#include "util/text.hpp"
#include "world/coords.hpp"

namespace rjc {
namespace {
uint32_t hmix(uint32_t a, uint32_t b) {
  uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}
}  // namespace

float App::bandKey() const {
  float k = band_on_ ? Audio::musicKey(band_track_) : 60.0f;
  while (k < 57.0f) k += 12.0f;
  return k;
}

void App::setGuitar(bool on) {
  if (on == life_.guitar) return;
  life_.guitar = on;
  if (!on) {
    setBand(false);
    audience_.clear();
  }
  toast(tr(on ? "guitar.out" : "guitar.away"));
}

void App::setBand(bool on) {
  if (on == band_on_) return;
  band_on_ = on;
  band_.clear();
  if (!on) return;
  // bandmates: residents with the band / instrument hobby (two of them, different each hour)
  const uint32_t seed = static_cast<uint32_t>(clock_.unixUtc() / 3600);
  std::vector<size_t> cand;
  for (size_t i = 0; i < town_.size(); ++i)
    for (auto h : town_.npc(i).hobbies)
      if (h == rj::sim::Hobby::Band || h == rj::sim::Hobby::Instrument) {
        cand.push_back(i);
        break;
      }
  for (int k = 0; k < 2 && !cand.empty(); ++k) {
    const size_t j = hmix(seed, static_cast<uint32_t>(k)) % cand.size();
    band_.push_back(cand[j]);
    cand.erase(cand.begin() + static_cast<long>(j));
  }
  band_track_ = static_cast<int>(seed % static_cast<uint32_t>(Audio::musicTracks()));
  std::string names;
  for (size_t i : band_) names += (names.empty() ? "" : "、") + town_.npc(i).fullName();
  toast(names.empty() ? tr("band.on_generic") : i18n_.f("band.on", {{"names", names}}));
}

void App::updateBusking(float dt) {
  if (!life_.guitar) return;
  if (driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_ || player_.fly) {
    setGuitar(false);
    return;
  }
  if (screen_ != Screen::Game || shop_open_ >= 0 || vend_open_ >= 0 || fuel_open_ >= 0) return;
  if (IsKeyPressed(KEY_J)) {
    setGuitar(false);
    return;
  }
  if (IsKeyPressed(KEY_N)) setBand(!band_on_);
  static const int deg[8] = {0, 2, 4, 7, 9, 12, 14, 16};  // major pentatonic, two octaves
  const float key = bandKey();
  const double now = GetTime();
  static const bool test = std::getenv("RJ_BUSK_TEST") != nullptr;  // test aid: play a steady tune by itself
  static double next_test = 0;
  int test_note = -1;
  if (test && now >= next_test) {
    next_test = now + 0.45;
    static int step = 0;
    static const int tune[8] = {0, 2, 4, 3, 4, 2, 1, 0};
    test_note = tune[step++ % 8];
  }
  for (int k = 0; k < 8; ++k)
    if (IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + k)) || k == test_note) {
      audio_.pluck(key + static_cast<float>(deg[k]), 0.9f);
      audio_.pluck(key + static_cast<float>(deg[k]) - 12.0f, 0.25f);  // (a lower string with it)
      note_times_.push_back(now);
    }
  while (note_times_.size() > 12) note_times_.erase(note_times_.begin());
  // the groove: how steady the last notes were (intervals of 0.18-1.1 s), and still playing
  float groove = 0.0f;
  if (note_times_.size() >= 4 && now - note_times_.back() < 3.0) {
    int good = 0, n = 0;
    for (size_t i = note_times_.size() > 8 ? note_times_.size() - 8 : 1; i < note_times_.size(); ++i, ++n) {
      const double iv = note_times_[i] - note_times_[i - 1];
      if (iv >= 0.18 && iv <= 1.1) ++good;
    }
    groove = n > 0 ? static_cast<float>(good) / static_cast<float>(n) : 0.0f;
  }
  if (band_on_ && now - (note_times_.empty() ? -99.0 : note_times_.back()) < 3.0) groove = std::min(1.0f, groove + 0.2f);
  // passers-by stop to listen
  bool by_station = false;
  for (const auto& st : trains_.stations())
    if (std::hypot(st.pos.x - player_.pos.x, st.pos.y - player_.pos.y) < 90.0) by_station = true;
  audience_t_ += dt;
  if (audience_t_ > 1.2f) {
    audience_t_ = 0.0f;
    if (groove > 0.25f) {
      const uint32_t tick = static_cast<uint32_t>(now * 10.0);
      for (const auto& [id, w] : peds_.walkers()) {
        if (audience_.count(id) || std::hypot(w.pos.x - player_.pos.x, w.pos.y - player_.pos.y) > 11.0) continue;
        const float chance = (0.08f + 0.25f * groove) * (band_on_ ? 1.8f : 1.0f) * (by_station ? 1.4f : 1.0f);
        if (static_cast<float>(hmix(tick, static_cast<uint32_t>(id)) % 1000u) < chance * 1000.0f) {
          audience_[id] = 0.0f;
          peds_.hold(id, 12.0f, player_.pos);
        }
      }
    }
  }
  for (auto it = audience_.begin(); it != audience_.end();) {
    const Walker* w = nullptr;
    for (const auto& [id, ww] : peds_.walkers())
      if (id == it->first) w = &ww;
    if (!w || it->second > 45.0f || std::hypot(w->pos.x - player_.pos.x, w->pos.y - player_.pos.y) > 16.0) {
      it = audience_.erase(it);
      continue;
    }
    const float t0 = it->second;
    it->second += dt;
    if (groove > 0.25f) peds_.hold(it->first, 3.0f, player_.pos);  // (they stay while the music goes on)
    if (t0 < 6.0f && it->second >= 6.0f && !tipped_.count(it->first) && groove > 0.35f) {
      const uint32_t h = hmix(static_cast<uint32_t>(it->first), static_cast<uint32_t>(clock_.unixUtc() / 60));
      if (static_cast<float>(h % 1000u) < (0.35f + 0.3f * groove + (band_on_ ? 0.15f : 0.0f)) * 1000.0f) {
        static const int64_t amounts[] = {10, 50, 100, 100, 100, 500, 100, 1000};
        const int64_t y = amounts[(h >> 10) % 8];
        if (ledger_ && ledger_->transfer(ledger_->externalAccount(), player_account_, y, rj::econ::TxCategory::Transfer, clock_.unixUtc(), tr("busk.memo")) ==
                           rj::econ::TxResult::Ok) {
          tipped_.insert(it->first);
          tips_session_ += y;
          life_.tips += y;
          audio_.cue(Cue::Coins, 0.7f);
          toast(i18n_.f("busk.tip", {{"yen", withCommas(y)}}));
        }
      }
    }
    ++it;
  }
}

void App::drawGuitar(const Camera3D& cam) {
  (void)cam;
  if (!life_.guitar || driving_.active() || ride_train_ >= 0 || ride_ferry_ >= 0 || ride_jet_ >= 0 || flying_) return;
  const double yaw = player_.yaw;
  const rj::geo::Vec3d f{std::sin(yaw), std::cos(yaw), 0.0}, r{std::cos(yaw), -std::sin(yaw), 0.0};
  auto at = [&](const rj::geo::Vec3d& c, double a, double b, double z) { return rj::geo::Vec3d{c.x + f.x * a + r.x * b, c.y + f.y * a + r.y * b, c.z + z}; };
  const bool fp = player_.camera_mode == 0;
  const rj::geo::Vec3d e = player_.eyeEnu();
  const rj::geo::Vec3d c = fp ? at(e, 0.42, 0.06, -0.55) : at(player_.pos, 0.2, 0.0, 1.02);
  const float fy = static_cast<float>(yaw);
  const Color wood{150, 86, 40, 255}, dark{30, 22, 18, 255};
  renderer_.drawBox(at(c, 0, 0.12, 0), fy, {0.2f, 0.05f, 0.17f}, 0, wood);            // body
  renderer_.drawBox(at(c, 0.052, 0.1, 0.01), fy, {0.05f, 0.003f, 0.05f}, 0, dark);    // sound hole
  renderer_.drawBox(at(c, 0.03, -0.26, 0.05), fy, {0.28f, 0.02f, 0.025f}, 0, dark);   // neck
  renderer_.drawBox(at(c, 0.03, -0.58, 0.08), fy, {0.07f, 0.025f, 0.035f}, 0, wood);  // head
  // the bandmates beside the player, facing the same way (one with a bass, one at a drum)
  for (size_t k = 0; k < band_.size() || (band_on_ && k < 2 && band_.empty()); ++k) {
    const double side = k == 0 ? -1.7 : 1.7;
    const rj::geo::Vec3d feet = at(player_.pos, -0.5, side, 0.0);
    renderer_.drawStandingPerson(enuToRl(feet), fy, static_cast<int>(3 + k), k == 0 ? Color{40, 40, 46, 255} : Color{160, 40, 50, 255}, Color{40, 44, 70, 255});
    if (k == 0) {
      renderer_.drawBox(at(feet, 0.22, 0.1, 1.0), fy, {0.22f, 0.05f, 0.14f}, 0, Color{30, 30, 34, 255});
      renderer_.drawBox(at(feet, 0.24, -0.3, 1.06), fy, {0.34f, 0.02f, 0.025f}, 0, Color{20, 20, 22, 255});
    } else {
      renderer_.drawBox(at(feet, 0.55, 0.0, 0.3), fy, {0.26f, 0.26f, 0.3f}, 0, Color{200, 200, 204, 255});   // drum
      renderer_.drawBox(at(feet, 0.55, 0.0, 0.61), fy, {0.25f, 0.25f, 0.01f}, 0, Color{240, 238, 230, 255}); // head
    }
  }
}

void App::drawBuskingHud() {
  if (!life_.guitar) return;
  const float w = 560, h = 118, x = 40, y = 1080 - 330;
  ui_.panel({x, y, w, h}, Color{10, 12, 18, 200});
  ui_.text(tr(input::padActive() ? "guitar.help_pad" : "guitar.help"), x + 18, y + 12, 20, theme::kText);
  ui_.text(i18n_.f("busk.status", {{"n", std::to_string(audience_.size())}, {"yen", withCommas(tips_session_)}}), x + 18, y + 46, 20, theme::kWarn);
  ui_.text(band_on_ ? i18n_.f("band.playing", {{"track", trackTitle(band_track_)}}) : tr("band.off"), x + 18, y + 80, 18, theme::kMuted);
}

}  // namespace rjc
