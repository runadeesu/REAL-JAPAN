#include "ui/ui.hpp"

#include <vector>

#include "platform/paths.hpp"

namespace rjc {

bool Ui::loadFont(const std::filesystem::path& ttf, const std::set<int>& codepoints, int size) {
  auto data = readFile(ttf);
  if (!data) return false;
  std::vector<int> cps(codepoints.begin(), codepoints.end());
  font_ = LoadFontFromMemory(".ttf", data->data(), static_cast<int>(data->size()), size, cps.data(),
                             static_cast<int>(cps.size()));
  if (font_.texture.id == 0 || font_.glyphCount == 0) return false;
  GenTextureMipmaps(&font_.texture);
  SetTextureFilter(font_.texture, TEXTURE_FILTER_TRILINEAR);
  has_font_ = true;
  return true;
}

void Ui::unload() {
  if (has_font_) UnloadFont(font_);
  has_font_ = false;
}

void Ui::beginFrame() {
  const float h = static_cast<float>(GetScreenHeight());
  const float w = static_cast<float>(GetScreenWidth());
  s_ = h / 1080.0f;
  vw_ = w / s_;
  mouse_ = GetMousePosition();
  clicked_ = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

float Ui::measure(const std::string& t, float size) const {
  const Font f = has_font_ ? font_ : GetFontDefault();
  return MeasureTextEx(f, t.c_str(), size * s_, 0.0f).x / s_;
}

void Ui::text(const std::string& t, float x, float y, float size, Color c) const {
  const Font f = has_font_ ? font_ : GetFontDefault();
  DrawTextEx(f, t.c_str(), {x * s_, y * s_}, size * s_, 0.0f, c);
}

void Ui::textCentered(const std::string& t, float cx, float y, float size, Color c) const {
  text(t, cx - measure(t, size) / 2, y, size, c);
}

void Ui::textRight(const std::string& t, float rx, float y, float size, Color c) const {
  text(t, rx - measure(t, size), y, size, c);
}

float Ui::textWrapped(const std::string& t, float x, float y, float width, float size, Color c) const {
  // Break on spaces for Latin text and between any characters for CJK.
  std::string line;
  float yy = y;
  size_t i = 0;
  while (i < t.size()) {
    const unsigned char ch = static_cast<unsigned char>(t[i]);
    const size_t n = ch < 0x80 ? 1 : (ch >> 5) == 0x6 ? 2 : (ch >> 4) == 0xE ? 3 : 4;
    std::string token = t.substr(i, n);
    if (ch == '\n') {
      text(line, x, yy, size, c);
      line.clear();
      yy += size * 1.35f;
      i += 1;
      continue;
    }
    if (ch < 0x80 && ch != ' ') {  // extend Latin words
      size_t j = i + 1;
      while (j < t.size() && static_cast<unsigned char>(t[j]) < 0x80 && t[j] != ' ' && t[j] != '\n') ++j;
      token = t.substr(i, j - i);
      i = j;
    } else {
      i += n;
    }
    if (measure(line + token, size) > width && !line.empty()) {
      text(line, x, yy, size, c);
      yy += size * 1.35f;
      line = (token == " ") ? "" : token;
    } else {
      line += token;
    }
  }
  if (!line.empty()) {
    text(line, x, yy, size, c);
    yy += size * 1.35f;
  }
  return yy - y;
}

void Ui::panel(Rectangle r, Color c) const { DrawRectangleRounded(px(r), 0.06f, 8, c); }

bool Ui::hovered(Rectangle r) const { return CheckCollisionPointRec(mouse_, px(r)); }

bool Ui::button(Rectangle r, const std::string& label, bool enabled, float size) {
  const bool hov = enabled && hovered(r);
  Color bg = hov ? Color{214, 0, 40, 230} : theme::kPanelLight;
  if (!enabled) bg = Color{40, 40, 46, 150};
  DrawRectangleRounded(px(r), 0.25f, 8, bg);
  if (hov) DrawRectangleRoundedLinesEx(px(r), 0.25f, 8, 2.0f * s_, Color{255, 255, 255, 120});
  textCentered(label, r.x + r.width / 2, r.y + (r.height - size) / 2 - 2, size, enabled ? theme::kText : theme::kMuted);
  if (hov && clicked_) {
    clicked_ = false;
    ++presses_;
    return true;
  }
  return false;
}

int Ui::stepper(Rectangle r, const std::string& label, const std::string& value) {
  panel(r, theme::kPanelLight);
  text(label, r.x + 24, r.y + (r.height - 30) / 2, 30, theme::kText);
  const Rectangle left{r.x + r.width - 430, r.y + 8, 64, r.height - 16};
  const Rectangle right{r.x + r.width - 80, r.y + 8, 64, r.height - 16};
  int d = 0;
  if (button(left, "<", true, 30)) d = -1;
  if (button(right, ">", true, 30)) d = +1;
  textCentered(value, (left.x + left.width + right.x) / 2, r.y + (r.height - 30) / 2, 30, theme::kText);
  return d;
}

}  // namespace rjc
