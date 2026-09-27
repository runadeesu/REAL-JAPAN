#pragma once
// Readings of the fictional island's station names (for the station name boards). The names are
// invented for this game; the readings are ours.

#include <string>

namespace rjc {

struct StationReading {
  const char* name;   // base name as it appears before 駅 in rail.txt
  const char* kana;
  const char* roman;
};

inline const StationReading* stationReading(const std::string& full) {
  static const StationReading kR[] = {
      {"千景中央", "ちかげちゅうおう", "Chikage-Chuo"}, {"天望台", "てんぼうだい", "Tembodai"}, {"古市", "ふるいち", "Furuichi"},
      {"臨海", "りんかい", "Rinkai"},                 {"西ヶ丘", "にしがおか", "Nishigaoka"}, {"山麓", "さんろく", "Sanroku"},
      {"千景北", "ちかげきた", "Chikage-Kita"},
  };
  for (const auto& r : kR)
    if (full.rfind(r.name, 0) == 0) return &r;
  return nullptr;
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

}  // namespace rjc
