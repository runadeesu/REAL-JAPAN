// What the controller's buttons and (touch builds) the on-screen buttons stand for in the situation
// at hand. The sticks / joystick, looking and "use" work the same everywhere (platform/input.cpp).

#include <string>
#include <utility>
#include <vector>

#include "app.hpp"

namespace rjc {

void App::updateInputContext() {
  const bool menu = screen_ != Screen::Game || shop_open_ >= 0 || fuel_open_ >= 0 || vend_open_ >= 0 || till_.on;
  input::PadMode m = input::PadMode::Walk;
  if (menu) m = input::PadMode::Menu;
  else if (flying_) m = input::PadMode::Fly;
  else if (drive_train_ >= 0) m = input::PadMode::Train;
  else if (driving_.active()) m = input::PadMode::Drive;
  else if (photo_mode_) m = input::PadMode::Photo;
  else if (fish_.stage > 0) m = input::PadMode::Fish;
  else if (life_.guitar && !driving_.active() && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0) m = input::PadMode::Play;
  input::setPadMode(m);

#if defined(RJ_TOUCH)
  std::vector<std::pair<int, std::string>> keys;
  auto add = [&](int key, const char* name) { keys.push_back({key, tr(std::string("touch.") + name)}); };
  if (!menu) {
    add(KEY_E, "use");
    if (flying_) {
      add(KEY_SPACE, "brake");
      add(KEY_LEFT_SHIFT, "throttle_up");
      add(KEY_LEFT_CONTROL, "throttle_down");
      add(KEY_Q, "rudder");
      add(KEY_F, "flaps_down");
      add(KEY_R, "flaps_up");
      add(KEY_V, "view");
    } else if (drive_train_ >= 0) {
      add(KEY_W, "notch_up");
      add(KEY_S, "notch_down");
      add(KEY_SPACE, "emergency");
    } else if (driving_.active()) {
      add(KEY_SPACE, "handbrake");
      add(KEY_H, "horn");
      add(KEY_V, "view");
      if (nearFuelStation()) add(KEY_F, "refuel");
    } else if (photo_mode_) {
      // (E takes the picture)
    } else if (fish_.stage > 0) {
      add(KEY_SPACE, "reel");
    } else if (life_.guitar && ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0) {
      keys.clear();  // the guitar: six notes, the band, put it away
      for (int k = 0; k < 6; ++k) keys.push_back({KEY_ONE + k, std::to_string(k + 1)});
      add(KEY_N, "band");
      add(KEY_J, "guitar_away");
    } else if (ride_train_ < 0 && ride_ferry_ < 0 && ride_jet_ < 0) {
      add(KEY_SPACE, "jump");
      add(KEY_V, "view");
      if (player_.fly) add(KEY_LEFT_CONTROL, "down");
      if (frame_ - shrine_frame_ <= 1) {
        add(KEY_O, "omikuji");
        add(KEY_G, "goshuin");
      }
    }
    add(KEY_TAB, "phone");
    add(KEY_ESCAPE, "menu");
  }
  std::vector<touch::Button> b;
  for (const auto& [k, label] : keys) b.push_back({k, label.c_str()});
  touch::setButtons(b.data(), static_cast<int>(b.size()));
#endif
}

}  // namespace rjc
