// On-screen touch controls (Android), behind platform/input.cpp. In look mode (the desktop's captured mouse) a finger on the
// left half is a floating joystick (W/A/S/D, pushed to the rim also Shift), a finger on the right
// half turns the view, a short tap there is a click (the crosshair's "use"), and the buttons on the
// right stand for keys. Outside look mode the first finger is simply the mouse pointer.
// On the Linux test build the mouse plays one finger and the keyboard keeps working.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "platform/input_internal.hpp"
#include "platform/input_shim.hpp"

namespace rjc::touch {
namespace {

enum class Role { Stick, Look, Button, Pointer };

struct Finger {
  int id = 0;
  Role role = Role::Pointer;
  Vector2 start{}, last{};
  double t0 = 0;
  float moved = 0;
  int button = -1;
  bool seen = false;
};

struct Placed {
  int key = 0;
  std::string label;
  Vector2 c{};
  float r = 0;
};

constexpr int kKeys = 400;
std::vector<Finger> fingers_;
std::vector<Placed> buttons;
bool look_mode = false;
bool held_[kKeys] = {}, prev[kKeys] = {};
Vector2 look_delta{};
bool tap = false;
bool stick_on = false;
Vector2 stick_origin{}, stick_vec{};  // vec: offset / radius, clamped to 1.35
float pinch_wheel = 0;
float last_pinch = -1;

float scale() { return static_cast<float>(GetScreenHeight()) / 1080.0f; }
float stickRadius() { return 120.0f * scale(); }

void layout() {
  // big "use" button bottom right, the rest in an arc around it; menu and phone at the top right
  const float s = scale(), W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
  static const Vector2 arc[] = {{-420, -120}, {-400, -330}, {-250, -470}, {-60, -500}, {-590, -110}, {-580, -330}, {-450, -540}};
  int k = 0, top = 0;
  for (auto& b : buttons) {
    if (b.key == KEY_ESCAPE || b.key == KEY_TAB) {
      b.r = 52 * s;
      b.c = {W - (80 + 135 * static_cast<float>(top++)) * s, 74 * s};
    } else if (k == 0) {
      b.r = 96 * s;
      b.c = {W - 200 * s, H - 210 * s};
      ++k;
    } else {
      const Vector2 o = arc[std::min(k - 1, 6)];
      b.r = 64 * s;
      b.c = {W - 200 * s + o.x * s, H - 210 * s + o.y * s};
      ++k;
    }
  }
}

int buttonAt(Vector2 p) {
  for (size_t i = 0; i < buttons.size(); ++i) {
    const float dx = p.x - buttons[i].c.x, dy = p.y - buttons[i].c.y;
    if (dx * dx + dy * dy < buttons[i].r * buttons[i].r * 1.3f) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

void setButtons(const Button* b, int n) {
  bool same = static_cast<int>(buttons.size()) == n;
  for (int i = 0; same && i < n; ++i) same = buttons[i].key == b[i].key && buttons[i].label == b[i].label;
  if (same) return;
  // a finger holding a button that goes away lets go of it
  for (auto& f : fingers_)
    if (f.role == Role::Button) f.button = -1;
  buttons.clear();
  for (int i = 0; i < n; ++i) buttons.push_back({b[i].key, b[i].label ? b[i].label : "", {}, 0});
}

void update(bool look) {
  if (look && !look_mode)  // fingers already down keep going, as look fingers
    for (auto& f : fingers_)
      if (f.role == Role::Pointer) f.role = Role::Look;
  look_mode = look;
  std::copy(std::begin(held_), std::end(held_), std::begin(prev));
  std::fill(std::begin(held_), std::end(held_), false);
  look_delta = {};
  tap = false;
  pinch_wheel = 0;
  layout();

  // current touch points (the test build's mouse is one finger while its button is held)
  struct Pt {
    int id;
    Vector2 p;
  };
  std::vector<Pt> pts;
  const int n = GetTouchPointCount();
  for (int i = 0; i < n; ++i) pts.push_back({GetTouchPointId(i), GetTouchPosition(i)});
#if !defined(__ANDROID__)
  if (n == 0 && !look_mode && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) pts.push_back({0, GetMousePosition()});
  if (n == 0 && look_mode && IsKeyDown(KEY_LEFT_ALT) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    // (test: Alt+drag is a finger on the screen while the mouse is captured)
    static Vector2 virt{};
    static double last = -1;
    if (GetTime() - last > 0.2) virt = GetMousePosition();
    last = GetTime();
    const Vector2 d = GetMouseDelta();
    virt.x += d.x;
    virt.y += d.y;
    pts.push_back({0, virt});
  }
#endif

  for (auto& f : fingers_) f.seen = false;
  const double now = GetTime();
  for (const Pt& t : pts) {
    auto it = std::find_if(fingers_.begin(), fingers_.end(), [&](const Finger& f) { return f.id == t.id; });
    if (it == fingers_.end()) {
      Finger f;
      f.id = t.id;
      f.start = f.last = t.p;
      f.t0 = now;
      if (!look_mode) {
        f.role = Role::Pointer;
      } else if (const int b = buttonAt(t.p); b >= 0) {
        f.role = Role::Button;
        f.button = b;
      } else if (t.p.x < 0.45f * static_cast<float>(GetScreenWidth()) && !stick_on) {
        f.role = Role::Stick;
        stick_on = true;
        stick_origin = t.p;
        stick_vec = {};
      } else {
        f.role = Role::Look;
      }
      fingers_.push_back(f);
      it = std::prev(fingers_.end());
    }
    Finger& f = *it;
    f.seen = true;
    const Vector2 d{t.p.x - f.last.x, t.p.y - f.last.y};
    f.moved += std::hypot(d.x, d.y);
    f.last = t.p;
    switch (f.role) {
      case Role::Look: {
        // a swipe across the whole screen turns the view about half a turn
        const float k = 1400.0f / std::max(1.0f, static_cast<float>(GetScreenWidth()));
        look_delta.x += d.x * k;
        look_delta.y += d.y * k;
        break;
      }
      case Role::Stick: {
        const float r = stickRadius();
        Vector2 v{(t.p.x - stick_origin.x) / r, (t.p.y - stick_origin.y) / r};
        const float m = std::hypot(v.x, v.y);
        if (m > 1.35f) {  // the base follows a finger that runs past the rim
          const float over = (m - 1.35f) / m;
          stick_origin.x += (t.p.x - stick_origin.x) * over;
          stick_origin.y += (t.p.y - stick_origin.y) * over;
          v.x *= 1.35f / m;
          v.y *= 1.35f / m;
        }
        stick_vec = v;
        break;
      }
      case Role::Button:
        if (f.button >= 0 && f.button < static_cast<int>(buttons.size())) held_[buttons[f.button].key] = true;
        break;
      case Role::Pointer:
        break;
    }
  }
  // lifted fingers
  for (auto it = fingers_.begin(); it != fingers_.end();) {
    if (it->seen) {
      ++it;
      continue;
    }
    if (it->role == Role::Stick) {
      stick_on = false;
      stick_vec = {};
    }
    if (it->role == Role::Look && it->moved < 22.0f * scale() && now - it->t0 < 0.35) tap = true;
    it = fingers_.erase(it);
  }
  if (stick_on) {
    const float x = stick_vec.x, y = stick_vec.y;
    if (y < -0.38f) held_[KEY_W] = true;
    if (y > 0.38f) held_[KEY_S] = true;
    if (x < -0.38f) held_[KEY_A] = true;
    if (x > 0.38f) held_[KEY_D] = true;
    if (std::hypot(x, y) > 1.15f) held_[KEY_LEFT_SHIFT] = true;  // pushed to the rim: run
  }
  // two fingers outside look mode: pinch = mouse wheel (zooms the phone's map)
  if (!look_mode && pts.size() == 2) {
    const float dist = std::hypot(pts[0].p.x - pts[1].p.x, pts[0].p.y - pts[1].p.y);
    if (last_pinch > 0 && std::fabs(dist - last_pinch) > 18.0f * scale()) {
      pinch_wheel = dist > last_pinch ? 1.0f : -1.0f;
      last_pinch = dist;
    } else if (last_pinch <= 0) {
      last_pinch = dist;
    }
  } else {
    last_pinch = -1;
  }
}

bool down(int key) { return key >= 0 && key < kKeys && held_[key]; }
bool pressed(int key) { return key >= 0 && key < kKeys && held_[key] && !prev[key]; }
bool tapped() { return tap; }
Vector2 lookDelta() { return look_delta; }
float pinchWheel() { return pinch_wheel; }
bool fingers() { return !fingers_.empty(); }

void draw(const Font& font) {
  if (!look_mode) return;
  const float s = scale();
  // joystick
  const float r = stickRadius();
  const Vector2 base = stick_on ? stick_origin : Vector2{250 * s, static_cast<float>(GetScreenHeight()) - 260 * s};
  DrawCircleV(base, r, Color{255, 255, 255, static_cast<unsigned char>(stick_on ? 46 : 24)});
  DrawCircleLinesV(base, r, Color{255, 255, 255, 90});
  DrawCircleLinesV(base, r * 1.15f, Color{255, 255, 255, static_cast<unsigned char>(stick_on && std::hypot(stick_vec.x, stick_vec.y) > 1.15f ? 150 : 30)});
  const float kx = std::clamp(stick_vec.x, -1.0f, 1.0f), ky = std::clamp(stick_vec.y, -1.0f, 1.0f);
  DrawCircleV({base.x + kx * r, base.y + ky * r}, r * 0.42f, Color{255, 255, 255, static_cast<unsigned char>(stick_on ? 140 : 60)});
  // buttons
  for (const auto& b : buttons) {
    const bool held = b.key >= 0 && b.key < kKeys && held_[b.key];
    DrawCircleV(b.c, b.r, held ? Color{214, 0, 40, 170} : Color{12, 14, 20, 120});
    DrawCircleLinesV(b.c, b.r, Color{255, 255, 255, 150});
    const float fs = b.r * (b.label.size() > 9 ? 0.34f : 0.44f);
    const Vector2 m = MeasureTextEx(font, b.label.c_str(), fs, 0);
    DrawTextEx(font, b.label.c_str(), {b.c.x - m.x / 2, b.c.y - m.y / 2}, fs, 0, Color{245, 245, 245, 235});
  }
}

}  // namespace rjc::touch
