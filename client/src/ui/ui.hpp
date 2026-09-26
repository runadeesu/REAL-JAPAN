#pragma once
// Immediate-mode UI helpers. Layout uses a virtual canvas 1080 units high
// (width = 1080 * aspect); everything scales with the window.

#include <filesystem>
#include <set>
#include <string>

#include "raylib.h"

namespace rjc {

namespace theme {
inline constexpr Color kPanel{12, 14, 20, 200};
inline constexpr Color kPanelLight{30, 34, 44, 215};
inline constexpr Color kAccent{214, 0, 40, 255};  // hinomaru red
inline constexpr Color kText{240, 240, 240, 255};
inline constexpr Color kMuted{170, 174, 184, 255};
inline constexpr Color kGood{90, 200, 120, 255};
inline constexpr Color kWarn{240, 190, 70, 255};
}  // namespace theme

class Ui {
 public:
  bool loadFont(const std::filesystem::path& ttf, const std::set<int>& codepoints, int size);
  void unload();
  bool hasFont() const { return has_font_; }

  void beginFrame();
  float scale() const { return s_; }
  float vw() const { return vw_; }  // virtual width
  float vh() const { return 1080.0f; }

  void text(const std::string& t, float x, float y, float size, Color c) const;
  void textCentered(const std::string& t, float cx, float y, float size, Color c) const;
  void textRight(const std::string& t, float rx, float y, float size, Color c) const;
  // Word-wraps at `width`; returns height used.
  float textWrapped(const std::string& t, float x, float y, float width, float size, Color c) const;
  float measure(const std::string& t, float size) const;

  void panel(Rectangle r, Color c = theme::kPanel) const;
  bool hovered(Rectangle r) const;
  bool button(Rectangle r, const std::string& label, bool enabled = true, float size = 34.0f);
  // Row with "< value >" arrows. Returns -1, 0 or +1.
  int stepper(Rectangle r, const std::string& label, const std::string& value);

  Rectangle px(Rectangle r) const { return {r.x * s_, r.y * s_, r.width * s_, r.height * s_}; }
  bool clicked() const { return clicked_; }
  void consumeClick() { clicked_ = false; }

  Font font() const { return font_; }

 private:
  Font font_{};
  bool has_font_ = false;
  float s_ = 1.0f;
  float vw_ = 1920.0f;
  bool clicked_ = false;
  Vector2 mouse_{};
};

}  // namespace rjc
