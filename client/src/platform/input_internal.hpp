#pragma once
// Between platform/input.cpp (the one input front for the game) and platform/touch.cpp.

#include "raylib.h"

#undef IsKeyDown
#undef IsKeyPressed
#undef IsMouseButtonDown
#undef IsMouseButtonPressed
#undef GetMouseDelta
#undef GetMousePosition
#undef GetMouseWheelMove
#undef DisableCursor
#undef EnableCursor

namespace rjc::touch {
void update(bool look_mode);  // track the fingers (look mode: joystick, look, buttons, taps)
bool down(int key);           // a key held by the on-screen controls
bool pressed(int key);
bool tapped();                // a short tap on the right half this frame (look mode)
Vector2 lookDelta();          // (mouse-like pixels)
float pinchWheel();           // two-finger pinch outside look mode
bool fingers();               // any finger on the screen
}  // namespace rjc::touch
