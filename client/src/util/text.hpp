#pragma once
// Small text utilities: key=value files, UTF-8 decoding, templating.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rjc {

// Parses "key = value" lines; '#' starts a comment line. "\n" in values
// becomes a newline. Later keys override earlier ones.
std::map<std::string, std::string> parseKeyValue(const std::string& text);
std::string trim(const std::string& s);
std::vector<std::string> splitWs(const std::string& s, size_t max_parts = 0);

void collectCodepoints(const std::string& utf8, std::set<int>& out);

// Replace {name} placeholders.
std::string fill(std::string tmpl, const std::vector<std::pair<std::string, std::string>>& args);

std::string withCommas(int64_t v);  // 1234567 -> "1,234,567"
std::string fixed(double v, int decimals);

}  // namespace rjc
