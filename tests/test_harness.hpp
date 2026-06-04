// Minimal, dependency-free test harness.
//
// Rationale: an HFT project should not drag in a heavyweight test framework for
// what amounts to a handful of value checks. This header gives us CHECK/REQUIRE
// macros, a tiny registration mechanism, and an HFT_TEST_MAIN() that runs all
// registered cases and returns non-zero on any failure (so CTest reports it).
//
// This is test-only code: allocations and STL use are fine here.
#pragma once

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <functional>

namespace hft::test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline int& failure_count() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back(Case{name, std::move(fn)});
  }
};

inline void report_failure(const char* file, int line, const std::string& msg) {
  std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, msg.c_str());
  ++failure_count();
}

// Approximate equality for floating point checks.
inline bool approx_eq(double a, double b, double rel = 1e-9, double abs_eps = 1e-12) {
  const double diff = std::fabs(a - b);
  if (diff <= abs_eps) return true;
  return diff <= rel * std::fmax(std::fabs(a), std::fabs(b));
}

inline int run_all() {
  int run = 0;
  for (auto& c : registry()) {
    const int before = failure_count();
    std::printf("[ RUN  ] %s\n", c.name);
    c.fn();
    const bool ok = failure_count() == before;
    std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
    ++run;
  }
  std::printf("--------------------------------------------------\n");
  std::printf("%d test case(s), %d failure(s)\n", run, failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace hft::test

// ---- Registration -----------------------------------------------------------
#define HFT_TEST(name)                                                          \
  static void name();                                                           \
  static ::hft::test::Registrar hft_reg_##name(#name, name);                    \
  static void name()

// ---- Assertions -------------------------------------------------------------
#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond))                                                                \
      ::hft::test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")");      \
  } while (0)

#define CHECK_EQ(a, b)                                                          \
  do {                                                                          \
    auto _va = (a);                                                             \
    auto _vb = (b);                                                             \
    if (!(_va == _vb))                                                          \
      ::hft::test::report_failure(__FILE__, __LINE__,                           \
        std::string("CHECK_EQ(" #a ", " #b ") -> ") +                          \
        std::to_string(_va) + " != " + std::to_string(_vb));                    \
  } while (0)

#define CHECK_NEAR(a, b, rel)                                                   \
  do {                                                                          \
    double _va = static_cast<double>(a);                                        \
    double _vb = static_cast<double>(b);                                        \
    if (!::hft::test::approx_eq(_va, _vb, (rel)))                               \
      ::hft::test::report_failure(__FILE__, __LINE__,                           \
        std::string("CHECK_NEAR(" #a ", " #b ") -> ") +                        \
        std::to_string(_va) + " vs " + std::to_string(_vb));                    \
  } while (0)

#define HFT_TEST_MAIN()                                                         \
  int main() { return ::hft::test::run_all(); }
