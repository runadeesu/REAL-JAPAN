#pragma once
// Readings of the fictional country's station and place names (for the station name boards and
// the car displays). The names are invented for this game; the readings come with the world data
// ("reading <name> <kana> <roman>" lines in rail.txt / client.txt).

#include <map>
#include <string>

namespace rjc {

struct StationReading {
  std::string name;   // base name as it appears before 駅 in rail.txt
  std::string kana;
  std::string roman;
};

inline std::map<std::string, StationReading>& stationReadings() {
  static std::map<std::string, StationReading> m;
  return m;
}

inline void addStationReading(const std::string& name, const std::string& kana, const std::string& roman) {
  stationReadings()[name] = StationReading{name, kana, roman};
}

// "千景中央駅（新幹線）" -> "千景中央"
inline std::string stationBaseName(const std::string& full) {
  std::string s = full;
  const auto p = s.find("（");
  if (p != std::string::npos) s = s.substr(0, p);
  const std::string eki = "駅";
  if (s.size() >= eki.size() && s.compare(s.size() - eki.size(), eki.size(), eki) == 0) s.resize(s.size() - eki.size());
  return s;
}

// The reading of a station's base name (exact match, else the longest known name it starts with).
inline const StationReading* stationReading(const std::string& full) {
  const std::string base = stationBaseName(full);
  const auto& m = stationReadings();
  auto it = m.find(base);
  if (it != m.end()) return &it->second;
  const StationReading* best = nullptr;
  for (const auto& [k, r] : m)
    if (base.rfind(k, 0) == 0 && (!best || k.size() > best->name.size())) best = &r;
  return best;
}

}  // namespace rjc
