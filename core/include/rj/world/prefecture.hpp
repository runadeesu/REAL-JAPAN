#pragma once
// The 47 prefectures (JIS X 0401), grouped into the 8 regions used by the
// REAL JAPAN spec, with the primary Plane Rectangular CS zone used by
// public-survey data in that prefecture.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rj::world {

enum class Region : uint8_t {
  Hokkaido,
  Tohoku,
  Kanto,
  Chubu,
  Kinki,
  Chugoku,
  Shikoku,
  KyushuOkinawa,
};

std::string_view regionNameJa(Region r);
std::string_view regionNameEn(Region r);

struct Prefecture {
  uint8_t code;              // JIS X 0401: 1..47
  std::string_view name_ja;  // e.g. "東京都"
  std::string_view name_en;  // e.g. "Tokyo"
  Region region;
  uint8_t primary_plane_zone;         // 1..19
  std::array<uint8_t, 3> extra_zones; // other zones used in parts of the prefecture (0 = none)
};

std::span<const Prefecture> allPrefectures();
std::optional<Prefecture> prefectureByCode(int code);
std::optional<Prefecture> prefectureByNameJa(std::string_view name_ja);
std::vector<Prefecture> prefecturesInRegion(Region r);

// JIS X 0402 municipality code check digit for a 5-digit code (e.g. 13113 -> 0).
int municipalityCheckDigit(int code5);

}  // namespace rj::world
