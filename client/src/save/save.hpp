#pragma once
// Save games: <userDir>/saves/slot{0..3}.sav (slot 0 = autosave).
// Plain UTF-8 key=value text so saves stay inspectable and forward-compatible.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rjc {

struct SaveGame {
  int version = 1;
  std::string slice = "shibuya";
  std::string saved_at;  // local wall-clock time
  double lat = 0.0, lon = 0.0, h = 0.0;  // player feet, JGD2011 geodetic (h: T.P.-based local height)
  float yaw_deg = 0.0f, pitch_deg = 0.0f;
  bool fly = false;
  int camera_mode = 0;  // 0 first person, 1 third person
  int64_t game_unix = 0;
  int64_t money = 0;
  double play_seconds = 0.0;
  std::string interior;  // id of the interior the player is in (empty = outside)
  std::string inventory;  // things bought in shops: "item:count;item:count"
};

constexpr int kAutosaveSlot = 0;
constexpr int kSaveSlots = 4;

std::filesystem::path savePath(int slot);
bool writeSave(int slot, const SaveGame& s);
std::optional<SaveGame> readSave(int slot);
// Most recent save across all slots (for "Continue").
std::optional<int> latestSlot();

}  // namespace rjc
