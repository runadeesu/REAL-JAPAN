#pragma once
// Force-included into every client source (except platform/paths.cpp, which must not see raylib.h
// on Windows): raylib's keyboard and mouse queries also see a game controller and, on the touch
// builds (Android; RJ_GLES on Linux for testing), the on-screen controls (platform/input.cpp).

#ifndef RJ_NO_INPUT_SHIM

#include "raylib.h"

namespace rjc::input {

// What the controller's buttons stand for: the app sets it every frame from the situation.
enum class PadMode { Walk, Drive, Fly, Train, Fish, Photo, Menu };

// Once per frame, before the game reads input.
void update();
void setPadMode(PadMode m);
// Look mode = the desktop's captured mouse (fingers and sticks steer); outside it the mouse, the
// first finger or the controller's cursor point at the menus.
void setLook(bool on);
bool look();
// A controller was the last thing used (hints show its buttons, the touch controls step aside).
bool padActive();
// The controller's pointer over the menus (drawn by the app last, in screen pixels).
void drawPointer();

bool keyDown(int key);
bool keyPressed(int key);
bool mouseDown(int button);
bool mousePressed(int button);
Vector2 mouseDelta();
Vector2 mousePosition();
float wheel();

// Test aid: a scripted controller instead of the real one (nullptr: back to the hardware).
struct PadState {
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;  // sticks -1..1 (y down), triggers 0..1
  bool a = false, b = false, x = false, y = false, lb = false, rb = false;
  bool back = false, start = false, l3 = false, r3 = false;
  bool up = false, down = false, left = false, right = false;
};
void injectPad(const PadState* s);

}  // namespace rjc::input

#if defined(RJ_TOUCH)
namespace rjc::touch {
struct Button {
  int key = 0;             // the keyboard key it stands for
  const char* label = "";  // (UTF-8, already translated)
};
// The buttons shown on the right while the game is in look mode (set by the app every frame).
void setButtons(const Button* b, int n);
// The on-screen controls (joystick, buttons), drawn over the HUD in screen pixels.
void draw(const Font& font);
}  // namespace rjc::touch
#endif

#ifndef RJ_INPUT_IMPL
#define IsKeyDown(k) ::rjc::input::keyDown(k)
#define IsKeyPressed(k) ::rjc::input::keyPressed(k)
#define IsMouseButtonDown(b) ::rjc::input::mouseDown(b)
#define IsMouseButtonPressed(b) ::rjc::input::mousePressed(b)
#define GetMouseDelta() ::rjc::input::mouseDelta()
#define GetMousePosition() ::rjc::input::mousePosition()
#define GetMouseWheelMove() ::rjc::input::wheel()
#define DisableCursor() ::rjc::input::setLook(true)
#define EnableCursor() ::rjc::input::setLook(false)
#endif

#endif  // RJ_NO_INPUT_SHIM
