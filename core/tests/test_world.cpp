#include "rj/world/address.hpp"
#include "rj/world/prefecture.hpp"
#include "rj_test.hpp"

using namespace rj::world;

RJ_TEST(all_47_prefectures_present) {
  const auto all = allPrefectures();
  RJ_CHECK_EQ(all.size(), 47u);
  for (size_t i = 0; i < all.size(); ++i) {
    RJ_CHECK_EQ(all[i].code, static_cast<uint8_t>(i + 1));
    RJ_CHECK(all[i].primary_plane_zone >= 1 && all[i].primary_plane_zone <= 19);
  }
  size_t total = 0;
  const int expected[] = {1, 6, 7, 9, 7, 5, 4, 8};  // spec's regional grouping
  for (int r = 0; r < 8; ++r) {
    const auto list = prefecturesInRegion(static_cast<Region>(r));
    RJ_CHECK_EQ(list.size(), static_cast<size_t>(expected[r]));
    total += list.size();
  }
  RJ_CHECK_EQ(total, 47u);
  RJ_CHECK_EQ(prefectureByCode(13)->name_ja, std::string_view("東京都"));
  RJ_CHECK_EQ(prefectureByCode(13)->primary_plane_zone, 9);
  RJ_CHECK_EQ(prefectureByCode(47)->primary_plane_zone, 15);
  RJ_CHECK_EQ(prefectureByNameJa("北海道")->code, 1);
  RJ_CHECK(!prefectureByCode(48).has_value());
}

RJ_TEST(municipality_check_digit) {
  RJ_CHECK_EQ(municipalityCheckDigit(1100), 2);   // 札幌市 011002
  RJ_CHECK_EQ(municipalityCheckDigit(13113), 0);  // 渋谷区 131130
  RJ_CHECK_EQ(municipalityCheckDigit(13100), 8);  // 特別区部 131008
}

RJ_TEST(address_hierarchy) {
  WorldAddress shibuya = WorldAddress::forMunicipality(13113);
  RJ_CHECK(shibuya.region == Region::Kanto);
  RJ_CHECK_EQ(shibuya.prefecture, 13);
  RJ_CHECK(shibuya.depth() == AddressLevel::Municipality);

  WorldAddress bldg = shibuya;
  bldg.cell = rj::geo::MeshCode::parse("53393586");
  bldg.building_id = "13113-bldg-000001";
  RJ_CHECK(bldg.depth() == AddressLevel::Building);
  RJ_CHECK(shibuya.contains(bldg));
  RJ_CHECK(!bldg.contains(shibuya));
  RJ_CHECK_EQ(bldg.toString(), std::string("JP/Kanto/13/13113/m53393586/13113-bldg-000001"));
  WorldAddress minato = WorldAddress::forMunicipality(13103);
  RJ_CHECK(!minato.contains(bldg));
}
