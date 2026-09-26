// Headless simulation of residents over one day using the rjcore systems.
// Usage: rj_slice_sim [residents.csv] [YYYY-MM-DD]
//
// residents.csv (produced by pipeline/cook_residents.py from real PLATEAU
// buildings): id,occupation,home_id,home_lat,home_lon,work_id,work_lat,work_lon
// Without a CSV a tiny built-in sample is used (clearly synthetic).

#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "rj/sim/schedule.hpp"

using namespace rj::sim;

namespace {

std::vector<ResidentSpec> loadCsv(const std::string& path) {
  std::vector<ResidentSpec> out;
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    std::stringstream ss(line);
    std::array<std::string, 8> f;
    for (auto& x : f) std::getline(ss, x, ',');
    ResidentSpec r;
    r.id = std::stoull(f[0]);
    r.world_seed = 20260926;
    r.occupation_id = f[1];
    r.home = {f[2], {std::stod(f[3]), std::stod(f[4])}};
    if (!f[5].empty()) r.work = {f[5], {std::stod(f[6]), std::stod(f[7])}};
    out.push_back(r);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<ResidentSpec> specs;
  if (argc > 1) specs = loadCsv(argv[1]);
  if (specs.empty()) {
    std::printf("(no CSV given: using a 3-resident SYNTHETIC sample)\n");
    specs = {{1, 1, {"home-a", {35.6555, 139.6985}}, {"office-a", {35.6600, 139.7030}}, "office_worker"},
             {2, 1, {"home-b", {35.6560, 139.6990}}, {"shop-a", {35.6590, 139.7005}}, "shop_clerk"},
             {3, 1, {"home-c", {35.6565, 139.6995}}, {"school-a", {35.6610, 139.6980}}, "high_school_student"}};
  }
  CivilDate date{2026, 9, 24};
  if (argc > 2) std::sscanf(argv[2], "%d-%d-%d", &date.y, &date.m, &date.d);

  std::vector<DailyPlan> plans;
  for (const auto& s : specs) {
    const Npc n = generateResident(s);
    DayContext ctx;
    ctx.date = date;
    plans.push_back(planDay(n, ctx));
  }
  std::printf("date %04d-%02d-%02d (%s), residents %zu\n", date.y, date.m, date.d,
              isHoliday(date) ? std::string(*holidayName(date)).c_str() : "平日/週末", specs.size());
  std::printf("hour  ");
  for (int t = 0; t < static_cast<int>(ActivityType::kCount); ++t)
    std::printf("%s ", std::string(activityNameJa(static_cast<ActivityType>(t))).c_str());
  std::printf("\n");
  for (int h = 0; h < 24; ++h) {
    std::map<ActivityType, int> count;
    for (const auto& p : plans)
      if (const Activity* a = p.at(h * 60 + 30)) ++count[a->type];
    std::printf("%02d:30 ", h);
    for (int t = 0; t < static_cast<int>(ActivityType::kCount); ++t) std::printf("%4d ", count[static_cast<ActivityType>(t)]);
    std::printf("\n");
  }
  return 0;
}
