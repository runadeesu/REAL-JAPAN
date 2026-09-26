#include "rj/world/prefecture.hpp"

namespace rj::world {
namespace {

using R = Region;

// Plane zone assignment per 平成14年国土交通省告示第9号. extra_zones lists
// zones covering outlying parts (e.g. Tokyo: Ogasawara XIV, Okinotorishima
// XVIII, Minamitorishima XIX).
constexpr std::array<Prefecture, 47> kPrefectures{{
    {1, "北海道", "Hokkaido", R::Hokkaido, 12, {11, 13, 0}},
    {2, "青森県", "Aomori", R::Tohoku, 10, {0, 0, 0}},
    {3, "岩手県", "Iwate", R::Tohoku, 10, {0, 0, 0}},
    {4, "宮城県", "Miyagi", R::Tohoku, 10, {0, 0, 0}},
    {5, "秋田県", "Akita", R::Tohoku, 10, {0, 0, 0}},
    {6, "山形県", "Yamagata", R::Tohoku, 10, {0, 0, 0}},
    {7, "福島県", "Fukushima", R::Tohoku, 9, {0, 0, 0}},
    {8, "茨城県", "Ibaraki", R::Kanto, 9, {0, 0, 0}},
    {9, "栃木県", "Tochigi", R::Kanto, 9, {0, 0, 0}},
    {10, "群馬県", "Gunma", R::Kanto, 9, {0, 0, 0}},
    {11, "埼玉県", "Saitama", R::Kanto, 9, {0, 0, 0}},
    {12, "千葉県", "Chiba", R::Kanto, 9, {0, 0, 0}},
    {13, "東京都", "Tokyo", R::Kanto, 9, {14, 18, 19}},
    {14, "神奈川県", "Kanagawa", R::Kanto, 9, {0, 0, 0}},
    {15, "新潟県", "Niigata", R::Chubu, 8, {0, 0, 0}},
    {16, "富山県", "Toyama", R::Chubu, 7, {0, 0, 0}},
    {17, "石川県", "Ishikawa", R::Chubu, 7, {0, 0, 0}},
    {18, "福井県", "Fukui", R::Chubu, 6, {0, 0, 0}},
    {19, "山梨県", "Yamanashi", R::Chubu, 8, {0, 0, 0}},
    {20, "長野県", "Nagano", R::Chubu, 8, {0, 0, 0}},
    {21, "岐阜県", "Gifu", R::Chubu, 7, {0, 0, 0}},
    {22, "静岡県", "Shizuoka", R::Chubu, 8, {0, 0, 0}},
    {23, "愛知県", "Aichi", R::Chubu, 7, {0, 0, 0}},
    {24, "三重県", "Mie", R::Kinki, 6, {0, 0, 0}},
    {25, "滋賀県", "Shiga", R::Kinki, 6, {0, 0, 0}},
    {26, "京都府", "Kyoto", R::Kinki, 6, {0, 0, 0}},
    {27, "大阪府", "Osaka", R::Kinki, 6, {0, 0, 0}},
    {28, "兵庫県", "Hyogo", R::Kinki, 5, {0, 0, 0}},
    {29, "奈良県", "Nara", R::Kinki, 6, {0, 0, 0}},
    {30, "和歌山県", "Wakayama", R::Kinki, 6, {0, 0, 0}},
    {31, "鳥取県", "Tottori", R::Chugoku, 5, {0, 0, 0}},
    {32, "島根県", "Shimane", R::Chugoku, 3, {0, 0, 0}},
    {33, "岡山県", "Okayama", R::Chugoku, 5, {0, 0, 0}},
    {34, "広島県", "Hiroshima", R::Chugoku, 3, {0, 0, 0}},
    {35, "山口県", "Yamaguchi", R::Chugoku, 3, {0, 0, 0}},
    {36, "徳島県", "Tokushima", R::Shikoku, 4, {0, 0, 0}},
    {37, "香川県", "Kagawa", R::Shikoku, 4, {0, 0, 0}},
    {38, "愛媛県", "Ehime", R::Shikoku, 4, {0, 0, 0}},
    {39, "高知県", "Kochi", R::Shikoku, 4, {0, 0, 0}},
    {40, "福岡県", "Fukuoka", R::KyushuOkinawa, 2, {0, 0, 0}},
    {41, "佐賀県", "Saga", R::KyushuOkinawa, 2, {0, 0, 0}},
    {42, "長崎県", "Nagasaki", R::KyushuOkinawa, 1, {0, 0, 0}},
    {43, "熊本県", "Kumamoto", R::KyushuOkinawa, 2, {0, 0, 0}},
    {44, "大分県", "Oita", R::KyushuOkinawa, 2, {0, 0, 0}},
    {45, "宮崎県", "Miyazaki", R::KyushuOkinawa, 2, {0, 0, 0}},
    {46, "鹿児島県", "Kagoshima", R::KyushuOkinawa, 2, {1, 0, 0}},
    {47, "沖縄県", "Okinawa", R::KyushuOkinawa, 15, {16, 17, 0}},
}};

}  // namespace

std::string_view regionNameJa(Region r) {
  switch (r) {
    case R::Hokkaido: return "北海道";
    case R::Tohoku: return "東北";
    case R::Kanto: return "関東";
    case R::Chubu: return "中部";
    case R::Kinki: return "近畿";
    case R::Chugoku: return "中国";
    case R::Shikoku: return "四国";
    case R::KyushuOkinawa: return "九州・沖縄";
  }
  return "?";
}

std::string_view regionNameEn(Region r) {
  switch (r) {
    case R::Hokkaido: return "Hokkaido";
    case R::Tohoku: return "Tohoku";
    case R::Kanto: return "Kanto";
    case R::Chubu: return "Chubu";
    case R::Kinki: return "Kinki";
    case R::Chugoku: return "Chugoku";
    case R::Shikoku: return "Shikoku";
    case R::KyushuOkinawa: return "KyushuOkinawa";
  }
  return "?";
}

std::span<const Prefecture> allPrefectures() { return kPrefectures; }

std::optional<Prefecture> prefectureByCode(int code) {
  if (code < 1 || code > 47) return std::nullopt;
  return kPrefectures[static_cast<size_t>(code - 1)];
}

std::optional<Prefecture> prefectureByNameJa(std::string_view name_ja) {
  for (const auto& p : kPrefectures)
    if (p.name_ja == name_ja) return p;
  return std::nullopt;
}

std::vector<Prefecture> prefecturesInRegion(Region r) {
  std::vector<Prefecture> out;
  for (const auto& p : kPrefectures)
    if (p.region == r) out.push_back(p);
  return out;
}

int municipalityCheckDigit(int code5) {
  const int w[5] = {6, 5, 4, 3, 2};
  int sum = 0;
  for (int k = 4; k >= 0; --k) {
    sum += (code5 % 10) * w[k];
    code5 /= 10;
  }
  return (11 - sum % 11) % 10;
}

}  // namespace rj::world
