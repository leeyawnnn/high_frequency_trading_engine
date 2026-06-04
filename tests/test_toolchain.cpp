// Phase 0 smoke test: proves the toolchain is wired up correctly.
//   - C++20 is actually in effect.
//   - The hft include path resolves.
//   - Optimization/define plumbing reaches translation units.
// Real functional tests arrive with each subsequent phase.
#include "test_harness.hpp"
#include "hft/version.hpp"

HFT_TEST(cpp20_in_effect) {
  // __cplusplus for C++20 is 202002L. AppleClang reports 202002 for -std=c++20.
  CHECK(__cplusplus >= 202002L);
}

HFT_TEST(version_is_exposed) {
  CHECK_EQ(hft::kVersionMajor, 0);
  CHECK(std::string(hft::version_string()) == "0.1.0");
}

HFT_TEST(platform_flag_is_consistent) {
  // kLinuxEngine must agree with the HFT_LINUX define the build system set.
#if defined(HFT_LINUX) && HFT_LINUX
  CHECK(hft::kLinuxEngine);
#else
  CHECK(!hft::kLinuxEngine);
#endif
}

HFT_TEST_MAIN()
