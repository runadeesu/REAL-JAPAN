#include "i18n/i18n.hpp"

#include "platform/paths.hpp"
#include "util/text.hpp"

namespace rjc {

bool I18n::load(const std::filesystem::path& dir, const std::string& code) {
  auto primary = readText(dir / (code + ".lang"));
  auto ja = readText(dir / "ja.lang");
  if (!primary && !ja) return false;
  code_ = primary ? code : "ja";
  strings_ = parseKeyValue(primary ? *primary : *ja);
  fallback_ = ja ? parseKeyValue(*ja) : std::map<std::string, std::string>{};
  return true;
}

const std::string& I18n::tr(const std::string& key) const {
  if (auto it = strings_.find(key); it != strings_.end()) return it->second;
  if (auto it = fallback_.find(key); it != fallback_.end()) return it->second;
  return key;
}

std::string I18n::f(const std::string& key, const std::vector<std::pair<std::string, std::string>>& args) const {
  return fill(tr(key), args);
}

void I18n::collectAllCodepoints(const std::filesystem::path& dir, std::set<int>& out) const {
  for (const auto& f : listFiles(dir)) {
    if (f.extension() != ".lang") continue;
    if (auto t = readText(f)) collectCodepoints(*t, out);
  }
}

}  // namespace rjc
