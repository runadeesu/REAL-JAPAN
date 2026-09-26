#include "rj/sim/npc.hpp"

#include <algorithm>
#include <array>

namespace rj::sim {
namespace {

using S = ShiftPattern;
using W = WorkplaceKind;

// Income column: GAME-BALANCE placeholders only (see OccupationDef).
constexpr std::array<OccupationDef, 51> kOccupations{{
    {"office_worker", "会社員", S::Day, kMonFri, false, W::Office, 300000},
    {"executive", "経営者", S::Flexible, kMonSat, false, W::Office, 600000},
    {"doctor", "医師", S::Day, kMonSat, false, W::Hospital, 900000},
    {"nurse", "看護師", S::Night, kEveryDay, true, W::Hospital, 320000},
    {"pharmacist", "薬剤師", S::Day, kMonSat, false, W::Shop, 380000},
    {"teacher", "教師", S::Early, kMonFri, false, W::School, 350000},
    {"childcare_worker", "保育士", S::Early, kMonSat, false, W::School, 250000},
    {"police_officer", "警察官", S::Night, kEveryDay, true, W::PublicOffice, 350000},
    {"firefighter", "消防士", S::Night, kEveryDay, true, W::PublicOffice, 340000},
    {"paramedic", "救急隊", S::Night, kEveryDay, true, W::PublicOffice, 340000},
    {"sdf_member", "自衛官", S::Day, kMonFri, false, W::PublicOffice, 300000},
    {"civil_servant", "公務員", S::Day, kMonFri, false, W::PublicOffice, 330000},
    {"lawyer", "弁護士", S::Day, kMonFri, false, W::Office, 700000},
    {"architect", "建築士", S::Day, kMonFri, false, W::Office, 420000},
    {"engineer", "エンジニア", S::Day, kMonFri, false, W::Office, 420000},
    {"programmer", "プログラマー", S::Flexible, kMonFri, false, W::Office, 400000},
    {"designer", "デザイナー", S::Flexible, kMonFri, false, W::Office, 330000},
    {"video_creator", "映像クリエイター", S::Flexible, kMonSat, false, W::Studio, 320000},
    {"youtuber", "YouTuber", S::Flexible, kEveryDay, true, W::Home, 250000},
    {"streamer", "配信者", S::Late, kEveryDay, true, W::Home, 220000},
    {"musician", "ミュージシャン", S::Late, kEveryDay, true, W::Venue, 220000},
    {"idol", "アイドル", S::Flexible, kEveryDay, true, W::Venue, 200000},
    {"actor", "俳優", S::Flexible, kEveryDay, true, W::Studio, 250000},
    {"voice_actor", "声優", S::Flexible, kMonSat, true, W::Studio, 230000},
    {"athlete", "スポーツ選手", S::Day, kEveryDay, true, W::Venue, 400000},
    {"hairdresser", "美容師", S::Late, 0b1111101, true, W::Shop, 240000},
    {"barber", "理容師", S::Day, 0b1111101, true, W::Shop, 230000},
    {"cook", "料理人", S::Late, 0b1111011, true, W::Restaurant, 260000},
    {"patissier", "パティシエ", S::Early, 0b1111011, true, W::Shop, 240000},
    {"fisher", "漁師", S::Early, kMonSat, true, W::Port, 280000},
    {"farmer", "農家", S::Early, kEveryDay, true, W::Farm, 250000},
    {"forester", "林業", S::Early, kMonSat, false, W::Outdoor, 260000},
    {"construction_worker", "建設業", S::Early, kMonSat, false, W::ConstructionSite, 320000},
    {"truck_driver", "トラック運転手", S::Night, kMonSat, true, W::Vehicle, 330000},
    {"taxi_driver", "タクシードライバー", S::Late, kEveryDay, true, W::Vehicle, 300000},
    {"bus_driver", "バス運転手", S::Early, kEveryDay, true, W::Vehicle, 310000},
    {"train_driver", "鉄道運転士", S::Early, kEveryDay, true, W::Station, 380000},
    {"conductor", "車掌", S::Late, kEveryDay, true, W::Station, 340000},
    {"pilot", "パイロット", S::Flexible, kEveryDay, true, W::Airport, 900000},
    {"cabin_attendant", "客室乗務員", S::Flexible, kEveryDay, true, W::Airport, 330000},
    {"sailor", "船員", S::Flexible, kEveryDay, true, W::Port, 400000},
    {"shop_clerk", "店員", S::Late, kEveryDay, true, W::Shop, 220000},
    {"factory_worker", "工場作業員", S::Early, kMonFri, false, W::Factory, 270000},
    {"mechanic", "整備士", S::Day, kMonSat, false, W::Factory, 290000},
    {"delivery_rider", "配達員", S::Flexible, kEveryDay, true, W::Outdoor, 230000},
    {"elementary_student", "小学生", S::School, kMonFri, false, W::School, 0},
    {"junior_high_student", "中学生", S::School, kMonFri, false, W::School, 0},
    {"high_school_student", "高校生", S::School, kMonFri, false, W::School, 0},
    {"university_student", "大学生", S::School, kMonFri, false, W::University, 0},
    {"vocational_student", "専門学校生", S::School, kMonFri, false, W::School, 0},
    {"retired", "退職者", S::None, 0, false, W::None, 150000},
}};

constexpr std::array<std::string_view, static_cast<size_t>(Hobby::kCount)> kHobbyJa{
    "ゲーム", "音楽", "楽器", "バンド", "ライブ", "映画", "アニメ", "漫画", "写真", "カメラ",
    "車", "バイク", "自転車", "スケートボード", "BMX", "フリースタイルスクーター",
    "インラインスケート", "サーフィン", "スキー", "スノーボード", "キャンプ", "登山", "釣り",
    "旅行", "鉄道", "飛行機", "ファッション", "料理", "カラオケ", "スポーツ"};

// Common surnames / given names used for fictional residents.
constexpr std::array<std::string_view, 24> kFamilyNames{
    "佐藤", "鈴木", "高橋", "田中", "伊藤", "渡辺", "山本", "中村", "小林", "加藤", "吉田", "山田",
    "佐々木", "山口", "松本", "井上", "木村", "林", "斎藤", "清水", "山崎", "森", "池田", "橋本"};
constexpr std::array<std::string_view, 24> kGivenNames{
    "陽翔", "蓮", "湊", "悠真", "大和", "結衣", "陽葵", "凛", "葵", "芽依", "翔太", "美咲",
    "健太", "由美", "直樹", "恵", "拓也", "彩", "誠", "千尋", "光", "薫", "遥", "航"};
constexpr std::array<std::string_view, 10> kGenres{"J-POP", "ロック", "ジャズ", "クラシック",
                                                   "ヒップホップ", "アニソン", "EDM", "演歌",
                                                   "シティポップ", "メタル"};
constexpr std::array<std::string_view, 10> kFoods{"ラーメン", "寿司", "焼肉", "カレー", "うどん",
                                                  "そば", "天ぷら", "餃子", "パスタ", "ハンバーグ"};

float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

}  // namespace

std::string_view hobbyNameJa(Hobby h) {
  const auto i = static_cast<size_t>(h);
  return i < kHobbyJa.size() ? kHobbyJa[i] : "?";
}

bool hobbyIsOutdoor(Hobby h) {
  switch (h) {
    case Hobby::Photography: case Hobby::Cycling: case Hobby::Skateboard: case Hobby::BMX:
    case Hobby::FreestyleScooter: case Hobby::InlineSkate: case Hobby::Surfing:
    case Hobby::Skiing: case Hobby::Snowboarding: case Hobby::Camping: case Hobby::Hiking:
    case Hobby::Fishing: case Hobby::Motorcycles: case Hobby::Sports:
      return true;
    default:
      return false;
  }
}

std::span<const OccupationDef> allOccupations() { return kOccupations; }

const OccupationDef* occupationById(std::string_view id) {
  for (const auto& o : kOccupations)
    if (o.id == id) return &o;
  return nullptr;
}

Npc generateResident(const ResidentSpec& spec) {
  Npc n;
  n.id = spec.id;
  n.seed = hashCombine(spec.world_seed, spec.id);
  Rng r(n.seed);
  n.family_name = std::string(kFamilyNames[r.next() % kFamilyNames.size()]);
  n.given_name = std::string(kGivenNames[r.next() % kGivenNames.size()]);
  n.occupation_id = spec.occupation_id;
  const OccupationDef* occ = occupationById(spec.occupation_id);

  if (spec.occupation_id == "elementary_student") n.age = r.range(6, 12);
  else if (spec.occupation_id == "junior_high_student") n.age = r.range(12, 15);
  else if (spec.occupation_id == "high_school_student") n.age = r.range(15, 18);
  else if (spec.occupation_id == "university_student" || spec.occupation_id == "vocational_student")
    n.age = r.range(18, 23);
  else if (spec.occupation_id == "retired") n.age = r.range(65, 90);
  else n.age = r.range(20, 64);

  n.personality = {clamp01(r.normal(0.5, 0.18)), clamp01(r.normal(0.5, 0.18)),
                   clamp01(r.normal(0.5, 0.18)), clamp01(r.normal(0.5, 0.18)),
                   clamp01(r.normal(0.5, 0.18))};
  n.wake_minute = std::clamp(static_cast<int>(r.normal(6.75 * 60, 40)), 4 * 60 + 30, 10 * 60);
  n.sleep_need_min = std::clamp(static_cast<int>(r.normal(7 * 60, 35)), 5 * 60, 9 * 60);
  n.home = spec.home;
  if (occ && occ->shift == ShiftPattern::School) n.school = spec.work;
  else n.work = spec.work;
  n.monthly_income_jpy = occ ? occ->monthly_income_jpy : 0;

  const int nh = r.range(1, 3);
  for (int i = 0; i < nh; ++i) {
    const auto h = static_cast<Hobby>(r.next() % static_cast<uint64_t>(Hobby::kCount));
    if (std::find(n.hobbies.begin(), n.hobbies.end(), h) == n.hobbies.end()) n.hobbies.push_back(h);
  }
  n.favorite_music_genre = std::string(kGenres[r.next() % kGenres.size()]);
  n.favorite_food = std::string(kFoods[r.next() % kFoods.size()]);
  n.health.physical = clamp01(r.normal(0.9, 0.08));
  return n;
}

}  // namespace rj::sim
