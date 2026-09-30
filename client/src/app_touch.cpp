// Touch builds (Android): the on-screen buttons for the situation at hand. The joystick (W/A/S/D,
// Shift at the rim), turning the view and tapping (a click) are always there in look mode.

#include <string>
#include <utility>
#include <vector>

#include "app.hpp"

namespace rjc {

void App::updateTouchControls() {
  std::vector<std::pair<int, std::string>> keys;
  auto add = [&](int key, const char* name) { keys.push_back({key, tr(std::string("touch.") + name)}); };
  if (screen_ == Screen::Game && shop_open_ < 0 && !till_.on) {
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
    } else if (photo_mode_) {
      // (E takes the picture; pinch is not available in look mode, the phone's camera zooms with the wheel)
    } else if (fish_.stage > 0) {
      add(KEY_SPACE, "reel");
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
}

}  // namespace rjc
