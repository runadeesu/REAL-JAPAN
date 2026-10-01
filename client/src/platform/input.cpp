// The game's one input front (platform/input_shim.hpp turns raylib's keyboard and mouse queries
// into these): the real keyboard and mouse, a game controller on every platform, and on the touch
// builds the on-screen controls (platform/touch.cpp).
//
// Controller (Xbox layout; the same positions on others). Walking: left stick walks (pushed all the
// way or L3: run), right stick looks, A jump, X use, Y first/third person, B crouch / fly down,
// Back phone, Start menu, D-pad = arrow keys, LB/RB omikuji/goshuin. Driving: RT accelerate,
// LT brake/reverse, left stick steers, A handbrake, B horn, X get out, LB fuel station. Light aircraft: left stick
// pitch/roll, RT/LT power, LB/RB rudder, D-pad up/down flaps, A brakes. Train driver: RT/LT notch
// up/down, A emergency brake, X doors. Fishing: A (or RT) reels. Camera: X shutter, LB/RB zoom.
// Menus and the phone: the left stick (or D-pad) moves a pointer, A clicks, B goes back, the right
// stick scrolls.

#include <algorithm>
#include <cmath>
#include <iterator>

#include "platform/input_internal.hpp"
#define RJ_INPUT_IMPL
#include "platform/input_shim.hpp"

namespace rjc::input {
namespace {

constexpr int kKeys = 400;
bool pad_held[kKeys] = {}, pad_prev[kKeys] = {};
bool look_mode = false;
PadMode mode = PadMode::Walk;
bool pad_active = false;
const PadState* injected = nullptr;
Vector2 pad_look{};
Vector2 pointer{-1, -1};
bool pad_click = false, pad_click_prev = false;
float pad_wheel = 0;

struct Pad {
  bool ok = false;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
  bool a = false, b = false, x = false, y = false, lb = false, rb = false;
  bool back = false, start = false, l3 = false, r3 = false;
  bool up = false, down = false, left = false, right = false;
};

float deadzone(float v) {
  const float a = std::fabs(v);
  if (a < 0.18f) return 0.0f;
  return std::copysign((a - 0.18f) / 0.82f, v);
}

Pad readPad() {
  Pad p;
  if (injected) {
    const PadState& s = *injected;
    p = {true, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, s.a, s.b, s.x, s.y, s.lb, s.rb, s.back, s.start, s.l3, s.r3, s.up, s.down, s.left, s.right};
    return p;
  }
  int g = -1;
  for (int i = 0; i < 4 && g < 0; ++i)
    if (IsGamepadAvailable(i)) g = i;
  if (g < 0) return p;
  p.ok = true;
  p.lx = deadzone(GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_X));
  p.ly = deadzone(GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_Y));
  p.rx = deadzone(GetGamepadAxisMovement(g, GAMEPAD_AXIS_RIGHT_X));
  p.ry = deadzone(GetGamepadAxisMovement(g, GAMEPAD_AXIS_RIGHT_Y));
  // triggers read -1 (released) .. 1; some pads report L2/R2 as buttons only
  p.lt = std::clamp((GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_TRIGGER) + 1.0f) * 0.5f, 0.0f, 1.0f);
  p.rt = std::clamp((GetGamepadAxisMovement(g, GAMEPAD_AXIS_RIGHT_TRIGGER) + 1.0f) * 0.5f, 0.0f, 1.0f);
  if (IsGamepadButtonDown(g, GAMEPAD_BUTTON_LEFT_TRIGGER_2)) p.lt = 1.0f;
  if (IsGamepadButtonDown(g, GAMEPAD_BUTTON_RIGHT_TRIGGER_2)) p.rt = 1.0f;
  auto b = [&](GamepadButton k) { return IsGamepadButtonDown(g, k); };
  p.a = b(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
  p.b = b(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
  p.x = b(GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
  p.y = b(GAMEPAD_BUTTON_RIGHT_FACE_UP);
  p.lb = b(GAMEPAD_BUTTON_LEFT_TRIGGER_1);
  p.rb = b(GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
  p.back = b(GAMEPAD_BUTTON_MIDDLE_LEFT);
  p.start = b(GAMEPAD_BUTTON_MIDDLE_RIGHT);
  p.l3 = b(GAMEPAD_BUTTON_LEFT_THUMB);
  p.r3 = b(GAMEPAD_BUTTON_RIGHT_THUMB);
  p.up = b(GAMEPAD_BUTTON_LEFT_FACE_UP);
  p.down = b(GAMEPAD_BUTTON_LEFT_FACE_DOWN);
  p.left = b(GAMEPAD_BUTTON_LEFT_FACE_LEFT);
  p.right = b(GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
  return p;
}

bool anyInput(const Pad& p) {
  return std::fabs(p.lx) + std::fabs(p.ly) + std::fabs(p.rx) + std::fabs(p.ry) > 0.05f || p.lt > 0.3f || p.rt > 0.3f || p.a || p.b || p.x ||
         p.y || p.lb || p.rb || p.back || p.start || p.l3 || p.r3 || p.up || p.down || p.left || p.right;
}

void mapPad(const Pad& p, float dt) {
  bool* k = pad_held;
  const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
  if (!look_mode) {
    // the menus: a pointer, A clicks, B goes back, the right stick scrolls
    if (pointer.x < 0) pointer = {W / 2, H / 2};
    const float sp = 900.0f * (H / 1080.0f) * dt;
    const float dx = p.lx + (p.right ? 1.0f : 0.0f) - (p.left ? 1.0f : 0.0f), dy = p.ly + (p.down ? 1.0f : 0.0f) - (p.up ? 1.0f : 0.0f);
    pointer.x = std::clamp(pointer.x + dx * std::fabs(dx) * sp, 0.0f, W - 1);
    pointer.y = std::clamp(pointer.y + dy * std::fabs(dy) * sp, 0.0f, H - 1);
    pad_click = p.a;
    if (p.b || p.start) k[KEY_ESCAPE] = true;
    if (p.back) k[KEY_TAB] = true;
    static float acc = 0;
    acc += -p.ry * dt * 6.0f;
    if (p.lb) acc -= dt * 4.0f;
    if (p.rb) acc += dt * 4.0f;
    if (std::fabs(acc) >= 1.0f) {
      pad_wheel = acc > 0 ? 1.0f : -1.0f;
      acc = 0;
    }
    return;
  }
  pad_click = false;
  // look: a curve for fine aiming; about half a turn a second at full tilt
  const float kLook = 1400.0f * dt;
  pad_look.x += std::copysign(std::pow(std::fabs(p.rx), 1.6f), p.rx) * kLook;
  pad_look.y += std::copysign(std::pow(std::fabs(p.ry), 1.6f), p.ry) * kLook;
  const bool fly = mode == PadMode::Fly;
  if (p.ly < -0.35f) k[KEY_W] = true;
  if (p.ly > 0.35f) k[KEY_S] = true;
  if (p.lx < -0.35f) k[KEY_A] = true;
  if (p.lx > 0.35f) k[KEY_D] = true;
  if (p.up) k[fly ? KEY_F : KEY_UP] = true;
  if (p.down) k[fly ? KEY_R : KEY_DOWN] = true;
  if (p.left) k[KEY_LEFT] = true;
  if (p.right) k[KEY_RIGHT] = true;
  const bool play = mode == PadMode::Play;  // (the guitar: the face buttons are notes)
  if (p.x) k[play ? KEY_THREE : KEY_E] = true;
  if (p.y) k[play ? KEY_FOUR : KEY_V] = true;
  if (p.back) k[KEY_TAB] = true;
  if (p.start) k[KEY_ESCAPE] = true;
  switch (mode) {
    case PadMode::Walk:
    case PadMode::Menu:
      if (p.a) k[KEY_SPACE] = true;
      if (p.b) k[KEY_C] = true;
      if (p.l3 || std::hypot(p.lx, p.ly) > 0.92f) k[KEY_LEFT_SHIFT] = true;
      if (p.lb) k[KEY_O] = true;
      if (p.rb) k[KEY_G] = true;
      if (p.rt > 0.5f) k[KEY_E] = true;
      break;
    case PadMode::Drive:
      if (p.rt > 0.2f) k[KEY_W] = true;
      if (p.lt > 0.2f) k[KEY_S] = true;
      if (p.a) k[KEY_SPACE] = true;
      if (p.b || p.r3) k[KEY_H] = true;
      if (p.lb) k[KEY_F] = true;  // (fuel station menu)
      break;
    case PadMode::Fly:
      if (p.rt > 0.2f) k[KEY_LEFT_SHIFT] = true;
      if (p.lt > 0.2f) k[KEY_LEFT_CONTROL] = true;
      if (p.lb) k[KEY_Q] = true;
      if (p.rb) k[KEY_E] = true;
      if (p.a) k[KEY_SPACE] = true;
      break;
    case PadMode::Train:
      if (p.rt > 0.5f) k[KEY_W] = true;
      if (p.lt > 0.5f) k[KEY_S] = true;
      if (p.a) k[KEY_SPACE] = true;
      break;
    case PadMode::Fish:
      if (p.a || p.rt > 0.3f) k[KEY_SPACE] = true;
      break;
    case PadMode::Play:  // eight notes: A B X Y, the shoulders and the triggers; R3 puts it away, L3 the band
      if (p.a) k[KEY_ONE] = true;
      if (p.b) k[KEY_TWO] = true;
      if (p.lb) k[KEY_FIVE] = true;
      if (p.rb) k[KEY_SIX] = true;
      if (p.lt > 0.5f) k[KEY_SEVEN] = true;
      if (p.rt > 0.5f) k[KEY_EIGHT] = true;
      if (p.r3) k[KEY_J] = true;
      if (p.l3) k[KEY_N] = true;
      break;
    case PadMode::Photo:
      if (p.a || p.rt > 0.5f) k[KEY_E] = true;
      if (p.lb) pad_wheel = -1.0f * dt * 8.0f;
      if (p.rb) pad_wheel = 1.0f * dt * 8.0f;
      break;
  }
}

}  // namespace

void injectPad(const PadState* s) { injected = s; }
void setPadMode(PadMode m) { mode = m; }
bool padActive() { return pad_active; }
bool look() { return look_mode; }

void setLook(bool on) {
  if (on == look_mode) return;
  look_mode = on;
#if !defined(__ANDROID__)
  if (on) DisableCursor();  // (desktop: capture the mouse as before)
  else EnableCursor();
#endif
  if (!on) pointer = {-1, -1};
}

void update() {
  std::copy(std::begin(pad_held), std::end(pad_held), std::begin(pad_prev));
  std::fill(std::begin(pad_held), std::end(pad_held), false);
  pad_look = {};
  pad_click_prev = pad_click;
  pad_wheel = 0;
  const float dt = std::min(GetFrameTime(), 0.1f);
  const Pad p = readPad();
  if (p.ok) {
    if (anyInput(p)) pad_active = true;
    mapPad(p, dt);
  }
  // the keyboard, the mouse or a finger takes over again
  const Vector2 md = ::GetMouseDelta();
  if (!injected && (GetKeyPressed() != 0 || std::fabs(md.x) + std::fabs(md.y) > 2.0f || ::IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) pad_active = false;
#if defined(RJ_TOUCH)
  touch::update(look_mode);
  if (touch::fingers()) pad_active = false;
#endif
}

bool keyDown(int key) {
  if (key >= 0 && key < kKeys && pad_held[key]) return true;
#if defined(RJ_TOUCH)
  if (touch::down(key)) return true;
#endif
  if (key == KEY_ESCAPE && ::IsKeyDown(KEY_BACK)) return true;
  return ::IsKeyDown(key);
}

bool keyPressed(int key) {
  if (key >= 0 && key < kKeys && pad_held[key] && !pad_prev[key]) return true;
#if defined(RJ_TOUCH)
  if (touch::pressed(key)) return true;
#endif
  if (key == KEY_ESCAPE && ::IsKeyPressed(KEY_BACK)) return true;  // the Android back button
  return ::IsKeyPressed(key);
}

bool mouseDown(int button) {
  if (button == MOUSE_BUTTON_LEFT && pad_click) return true;
  if (look_mode) {
#if defined(__ANDROID__)
    return false;  // (fingers in look mode are joystick, look and buttons, not a mouse)
#elif defined(RJ_TOUCH)
    return !::IsKeyDown(KEY_LEFT_ALT) && ::IsMouseButtonDown(button);
#endif
  }
  return ::IsMouseButtonDown(button);
}

bool mousePressed(int button) {
  if (button == MOUSE_BUTTON_LEFT && pad_click && !pad_click_prev) return true;
  if (look_mode) {
#if defined(RJ_TOUCH)
    if (button == MOUSE_BUTTON_LEFT && touch::tapped()) return true;
#endif
#if defined(__ANDROID__)
    return false;
#elif defined(RJ_TOUCH)
    return !::IsKeyDown(KEY_LEFT_ALT) && ::IsMouseButtonPressed(button);
#endif
  }
  return ::IsMouseButtonPressed(button);
}

Vector2 mouseDelta() {
  Vector2 d = pad_look;
#if defined(RJ_TOUCH)
  const Vector2 t = touch::lookDelta();
  d.x += t.x;
  d.y += t.y;
#if defined(__ANDROID__)
  if (look_mode) return d;  // (the first finger is not a mouse there)
#else
  if (look_mode && ::IsKeyDown(KEY_LEFT_ALT)) return d;
#endif
#endif
  const Vector2 m = ::GetMouseDelta();
  return {d.x + m.x, d.y + m.y};
}

Vector2 mousePosition() {
  if (pad_active && !look_mode && pointer.x >= 0) return pointer;
  return ::GetMousePosition();
}

float wheel() {
  float w = ::GetMouseWheelMove() + pad_wheel;
#if defined(RJ_TOUCH)
  w += touch::pinchWheel();
#endif
  return w;
}

void drawPointer() {
  if (!pad_active || look_mode || pointer.x < 0) return;
  const float s = static_cast<float>(GetScreenHeight()) / 1080.0f;
  const Vector2 p = pointer;
  const Vector2 a{p.x, p.y}, b{p.x + 14 * s, p.y + 34 * s}, c{p.x + 30 * s, p.y + 22 * s};
  DrawTriangle(a, b, c, Color{255, 255, 255, 235});
  DrawTriangleLines(a, b, c, Color{20, 20, 24, 255});
  DrawCircleV(p, 3 * s, Color{214, 0, 40, 255});
}

}  // namespace rjc::input
