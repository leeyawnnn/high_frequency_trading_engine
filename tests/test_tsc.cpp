// Phase 1: TSC timestamping + calibration.
#include "test_harness.hpp"
#include "hft/tsc.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

HFT_TEST(calibration_is_sane) {
  const auto& cal = hft::tsc_calibration();
  // ns_per_cycle must be positive and finite, and within a plausible band:
  // x86 invariant TSC ~ 0.2..2 ns/cycle; Apple cntvct ~ 41 ns/tick. Allow a
  // wide window so this is a sanity gate, not a hardware-specific assertion.
  CHECK(cal.ns_per_cycle > 0.0);
  CHECK(cal.ns_per_cycle < 1000.0);
  CHECK(cal.cycles_per_ns > 0.0);
  // ns_per_cycle and cycles_per_ns must be reciprocals.
  CHECK_NEAR(cal.ns_per_cycle * cal.cycles_per_ns, 1.0, 1e-9);
  CHECK(cal.hz > 0);
}

HFT_TEST(counter_is_monotonic) {
  // The counter must be non-decreasing across many reads.
  std::uint64_t prev = hft::tsc_now();
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t cur = hft::tsc_now();
    CHECK(cur >= prev);
    prev = cur;
  }
}

HFT_TEST(counter_advances_over_time) {
  // After a real 1ms sleep the counter must have strictly advanced.
  const std::uint64_t a = hft::tsc_now();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const std::uint64_t b = hft::tsc_now();
  CHECK(b > a);
}

HFT_TEST(delta_matches_wall_clock) {
  // Measure a known ~30ms interval with both clocks; converted TSC delta should
  // agree with steady_clock to within a few percent.
  using clock = std::chrono::steady_clock;
  const auto w0 = clock::now();
  const std::uint64_t c0 = hft::tsc_now_serialized();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const std::uint64_t c1 = hft::tsc_now_serialized();
  const auto w1 = clock::now();

  const double wall_ns =
      std::chrono::duration<double, std::nano>(w1 - w0).count();
  const double tsc_ns = hft::tsc_to_ns(c1 - c0);

  // 5% tolerance: scheduler jitter on the sleep dominates the error budget.
  CHECK_NEAR(tsc_ns, wall_ns, 0.05);
}

HFT_TEST(tsc_to_ns_rounds) {
  const auto& cal = hft::tsc_calibration();
  const std::uint64_t cycles = 1000;
  const double exact = hft::tsc_to_ns(cycles);
  const std::uint64_t rounded = hft::tsc_to_ns_u(cycles);
  CHECK_NEAR(static_cast<double>(rounded), exact, 0.01 + 1.0 / (exact + 1.0));
  CHECK(cal.ns_per_cycle * 1000.0 > 0.0);
}

HFT_TEST_MAIN()
