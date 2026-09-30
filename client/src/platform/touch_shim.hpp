#pragma once
// Touch builds (Android; RJ_GLES on Linux to test them with a mouse): force-included into every
// client source, so raylib's keyboard and mouse queries also see the on-screen controls
// (platform/touch.cpp). Desktop builds never include this file.

#include "raylib.h"

namespace rjc::touch {

struct Button {
  int key = 0;            // the keyboard key it stands for
  const char* label = ""; // (UTF-8, already translated)
};

// Once per frame, before the game reads input: tracks the fingers and turns them into keys,
// mouse look and taps.
void update();
// The on-screen controls (joystick, buttons), drawn over the HUD in screen pixels.
void draw(const Font& font);
// The buttons shown on the right while the game is in look mode (set by the app every frame).
void setButtons(const Button* b, int n);
// Look mode = the desktop's captured mouse: fingers steer and walk. Outside it the first finger
// is the mouse pointer (menus, the phone, the map).
void setLook(bool on);
bool look();

bool keyDown(int key);
bool keyPressed(int key);
bool mouseDown(int button);
bool mousePressed(int button);
Vector2 mouseDelta();
float wheel();

}  // namespace rjc::touch

#ifndef RJ_TOUCH_IMPL
#define IsKeyDown(k) ::rjc::touch::keyDown(k)
#define IsKeyPressed(k) ::rjc::touch::keyPressed(k)
#define IsMouseButtonDown(b) ::rjc::touch::mouseDown(b)
#define IsMouseButtonPressed(b) ::rjc::touch::mousePressed(b)
#define GetMouseDelta() ::rjc::touch::mouseDelta()
#define GetMouseWheelMove() ::rjc::touch::wheel()
#define DisableCursor() ::rjc::touch::setLook(true)
#define EnableCursor() ::rjc::touch::setLook(false)
#endif
