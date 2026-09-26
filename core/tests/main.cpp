#include <cstdio>

#include "rj_test.hpp"

int main() {
  int failed_cases = 0;
  for (const auto& c : rj::test::registry()) {
    const int before = rj::test::failures();
    c.fn();
    const bool ok = rj::test::failures() == before;
    if (!ok) ++failed_cases;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("\n%zu test cases, %d failed, %d failed checks\n", rj::test::registry().size(), failed_cases,
              rj::test::failures());
  return failed_cases == 0 ? 0 : 1;
}
