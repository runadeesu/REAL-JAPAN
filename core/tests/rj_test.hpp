#pragma once
// Minimal self-contained test harness (no external dependencies so the core
// builds anywhere, including MinGW cross builds).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rj::test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace rj::test

#define RJ_CAT2(a, b) a##b
#define RJ_CAT(a, b) RJ_CAT2(a, b)
#define RJ_TEST(name)                                                              \
  static void name();                                                              \
  static ::rj::test::Registrar RJ_CAT(rj_reg_, name)(#name, name);                 \
  static void name()

#define RJ_CHECK(cond)                                                             \
  do {                                                                             \
    if (!(cond)) ::rj::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");         \
  } while (0)

#define RJ_CHECK_EQ(a, b)                                                          \
  do {                                                                             \
    if (!((a) == (b)))                                                             \
      ::rj::test::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")");            \
  } while (0)

#define RJ_CHECK_NEAR(a, b, tol)                                                   \
  do {                                                                             \
    const double rj_a_ = (a), rj_b_ = (b);                                         \
    if (!(std::fabs(rj_a_ - rj_b_) <= (tol)))                                      \
      ::rj::test::fail(__FILE__, __LINE__,                                         \
                       "CHECK_NEAR(" #a ", " #b ") got " + std::to_string(rj_a_) + \
                           " vs " + std::to_string(rj_b_));                        \
  } while (0)
