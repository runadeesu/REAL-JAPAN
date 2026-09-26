#include "util/text.hpp"

#include <cstdio>
#include <sstream>

namespace rjc {

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

std::map<std::string, std::string> parseKeyValue(const std::string& text) {
  std::map<std::string, std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(t.substr(0, eq));
    std::string val = trim(t.substr(eq + 1));
    std::string unesc;
    for (size_t i = 0; i < val.size(); ++i) {
      if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'n') {
        unesc.push_back('\n');
        ++i;
      } else {
        unesc.push_back(val[i]);
      }
    }
    out[key] = unesc;
  }
  return out;
}

std::vector<std::string> splitWs(const std::string& s, size_t max_parts) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) break;
    if (max_parts && out.size() + 1 == max_parts) {
      out.push_back(trim(s.substr(i)));
      break;
    }
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

void collectCodepoints(const std::string& s, std::set<int>& out) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int cp = 0, n = 0;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
    else { ++i; continue; }
    if (i + n > s.size()) break;
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.insert(cp);
    i += n;
  }
}

std::string fill(std::string t, const std::vector<std::pair<std::string, std::string>>& args) {
  for (const auto& [k, v] : args) {
    const std::string key = "{" + k + "}";
    size_t pos = 0;
    while ((pos = t.find(key, pos)) != std::string::npos) {
      t.replace(pos, key.size(), v);
      pos += v.size();
    }
  }
  return t;
}

std::string withCommas(int64_t v) {
  const bool neg = v < 0;
  std::string d = std::to_string(neg ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = d.rbegin(); it != d.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (neg) out.push_back('-');
  return std::string(out.rbegin(), out.rend());
}

std::string fixed(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

}  // namespace rjc
