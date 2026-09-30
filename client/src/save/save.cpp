#include "save/save.hpp"

#include <cstdio>
#include <sstream>

#include "platform/paths.hpp"
#include "util/text.hpp"

namespace rjc {

std::filesystem::path savePath(int slot) {
  return userDir() / "saves" / ("slot" + std::to_string(slot) + ".sav");
}

bool writeSave(int slot, const SaveGame& s) {
  char buf[64];
  std::ostringstream o;
  o << "# PROJECT: REAL JAPAN save\n";
  o << "version = " << s.version << "\n";
  o << "slice = " << s.slice << "\n";
  o << "saved_at = " << s.saved_at << "\n";
  std::snprintf(buf, sizeof buf, "%.9f", s.lat);
  o << "lat = " << buf << "\n";
  std::snprintf(buf, sizeof buf, "%.9f", s.lon);
  o << "lon = " << buf << "\n";
  std::snprintf(buf, sizeof buf, "%.3f", s.h);
  o << "h = " << buf << "\n";
  o << "yaw_deg = " << s.yaw_deg << "\n";
  o << "pitch_deg = " << s.pitch_deg << "\n";
  o << "fly = " << (s.fly ? 1 : 0) << "\n";
  o << "camera_mode = " << s.camera_mode << "\n";
  o << "game_unix = " << s.game_unix << "\n";
  o << "money = " << s.money << "\n";
  o << "play_seconds = " << s.play_seconds << "\n";
  o << "interior = " << s.interior << "\n";
  o << "inventory = " << s.inventory << "\n";
  return writeFileAtomic(savePath(slot), o.str());
}

std::optional<SaveGame> readSave(int slot) {
  auto t = readText(savePath(slot));
  if (!t) return std::nullopt;
  const auto kv = parseKeyValue(*t);
  auto get = [&](const char* k) -> std::string {
    auto it = kv.find(k);
    return it == kv.end() ? std::string{} : it->second;
  };
  SaveGame s;
  try {
    s.version = std::stoi(get("version"));
    s.slice = get("slice");
    s.saved_at = get("saved_at");
    s.lat = std::stod(get("lat"));
    s.lon = std::stod(get("lon"));
    s.h = std::stod(get("h"));
    s.yaw_deg = std::stof(get("yaw_deg"));
    s.pitch_deg = std::stof(get("pitch_deg"));
    s.fly = get("fly") == "1";
    s.camera_mode = std::stoi(get("camera_mode"));
    s.game_unix = std::stoll(get("game_unix"));
    s.money = std::stoll(get("money"));
    s.play_seconds = std::stod(get("play_seconds"));
    s.interior = get("interior");
    s.inventory = get("inventory");
  } catch (...) {
    return std::nullopt;
  }
  if (s.version != 1) return std::nullopt;
  return s;
}

std::optional<int> latestSlot() {
  std::optional<int> best;
  std::filesystem::file_time_type best_t{};
  for (int i = 0; i < kSaveSlots; ++i) {
    std::error_code ec;
    const auto p = savePath(i);
    if (!std::filesystem::exists(p, ec) || !readSave(i)) continue;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (!best || t > best_t) {
      best = i;
      best_t = t;
    }
  }
  return best;
}

}  // namespace rjc
