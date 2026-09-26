#pragma once
// Localisation: data/lang/<code>.lang files (key = value, UTF-8).
// Missing keys fall back to Japanese, then to the key itself, so a missing
// translation is visible instead of silently blank.

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rjc {

class I18n {
 public:
  bool load(const std::filesystem::path& lang_dir, const std::string& code);
  const std::string& code() const { return code_; }
  const std::string& tr(const std::string& key) const;
  std::string f(const std::string& key, const std::vector<std::pair<std::string, std::string>>& args) const;
  // Every codepoint used by any language file (for font atlas generation).
  void collectAllCodepoints(const std::filesystem::path& lang_dir, std::set<int>& out) const;

 private:
  std::string code_ = "ja";
  std::map<std::string, std::string> strings_;
  std::map<std::string, std::string> fallback_;
};

}  // namespace rjc
