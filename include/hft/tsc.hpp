// Cycle-counter timestamping with one-time calibration against steady_clock.
//
// Why a raw cycle counter and not std::chrono in the hot path?
//   - rdtsc / cntvct is a handful of cycles with no syscall; clock_gettime,
//     even via vDSO, is ~10-20ns and can serialize more than we want.
//   - We only need a *delta* in the hot path, so we stamp raw cycles on the
//     hot path (tsc_now()) and convert to nanoseconds off the hot path
//     (tsc_to_ns()), once, at report time.
//
// Architecture notes:
//   - x86-64: __rdtsc() reads the invariant TSC. On modern CPUs this ticks at a
//     fixed rate independent of turbo/frequency scaling — ideal for timing.
//   - aarch64: the generic timer virtual count (cntvct_el0) is the analogue.
//     On Apple Silicon it ticks at cntfrq_el0 ≈ 24 MHz, i.e. ~41 ns/tick, so
//     local (macOS/ARM) resolution is coarse. That is fine for development
//     sanity checks; the real sub-microsecond measurements happen on x86 Linux
//     where the TSC resolution is sub-nanosecond.
//
// Calibration is done once, lazily, off the hot path (Meyers singleton → the
// first caller pays for it; thread-safe via static-init guarantees).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  include <x86intrin.h>
#  define HFT_TSC_X86 1
#else
#  define HFT_TSC_X86 0
#endif

#include "hft/compiler.hpp"

namespace hft {

// ---------------------------------------------------------------------------
// Raw cycle counter reads.
// ---------------------------------------------------------------------------

// Fast, non-serializing read. Use this on the hot path for entry/exit stamps.
// The CPU may reorder it slightly w.r.t. surrounding instructions; for stage
// latencies that span thousands of cycles this is in the noise.
HFT_ALWAYS_INLINE std::uint64_t tsc_now() noexcept {
#if HFT_TSC_X86
  return __rdtsc();
#elif defined(__aarch64__)
  std::uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  // Portable fallback: steady_clock in nanoseconds (slow; non-target only).
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Serializing read: prevents the CPU from moving instructions across it.
// Use for calibration and for microbenchmarks that measure short spans.
HFT_ALWAYS_INLINE std::uint64_t tsc_now_serialized() noexcept {
#if HFT_TSC_X86
  unsigned aux;
  // rdtscp waits for prior loads/stores to retire; lfence stops later ops from
  // starting before the timestamp is taken.
  std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#elif defined(__aarch64__)
  // NOTE: in AArch64 inline asm ';' begins a comment, so instructions MUST be
  // separated by "\n\t" — a "isb; mrs ...; isb" string silently drops the mrs.
  // The leading isb prevents the counter read from being speculated early.
  std::uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#else
  return tsc_now();
#endif
}

// ---------------------------------------------------------------------------
// Calibration.
// ---------------------------------------------------------------------------

struct TscCalibration {
  double ns_per_cycle = 1.0;   // multiply a cycle-delta by this to get ns
  double cycles_per_ns = 1.0;  // inverse
  std::uint64_t hz = 0;        // estimated counter frequency
};

namespace detail {

inline TscCalibration calibrate_tsc() {
  using clock = std::chrono::steady_clock;
  constexpr int kRounds = 9;
  constexpr auto kWindow = std::chrono::milliseconds(5);

  std::array<double, kRounds> ns_per_cycle{};

  // Warm up instruction/data caches and the steady_clock path.
  (void)tsc_now_serialized();
  (void)clock::now();

  for (int i = 0; i < kRounds; ++i) {
    const auto w0 = clock::now();
    const std::uint64_t c0 = tsc_now_serialized();
    const auto deadline = w0 + kWindow;
    // Busy-spin rather than sleep: sleeping risks core migration / frequency
    // changes mid-window which would corrupt the ratio.
    while (clock::now() < deadline) {
    }
    const std::uint64_t c1 = tsc_now_serialized();
    const auto w1 = clock::now();

    const double ns =
        std::chrono::duration<double, std::nano>(w1 - w0).count();
    const double cycles = static_cast<double>(c1 - c0);
    ns_per_cycle[static_cast<std::size_t>(i)] =
        (cycles > 0.0) ? (ns / cycles) : 1.0;
  }

  std::sort(ns_per_cycle.begin(), ns_per_cycle.end());
  const double median = ns_per_cycle[kRounds / 2];  // robust to outliers

  TscCalibration cal;
  cal.ns_per_cycle = median;
  cal.cycles_per_ns = (median > 0.0) ? (1.0 / median) : 1.0;
  cal.hz = static_cast<std::uint64_t>((median > 0.0) ? (1e9 / median) : 0.0);
  return cal;
}

}  // namespace detail

// Calibrated once on first call; thread-safe.
inline const TscCalibration& tsc_calibration() {
  static const TscCalibration cal = detail::calibrate_tsc();
  return cal;
}

// Call once at startup to pay calibration cost up front (off the hot path),
// rather than on the first reporting call.
inline void tsc_calibrate() { (void)tsc_calibration(); }

// ---------------------------------------------------------------------------
// Conversion (cold path: reporting only).
// ---------------------------------------------------------------------------

// Convert a cycle *delta* to nanoseconds.
HFT_ALWAYS_INLINE double tsc_to_ns(std::uint64_t cycles) noexcept {
  return static_cast<double>(cycles) * tsc_calibration().ns_per_cycle;
}

// Same, rounded to whole nanoseconds (what the histogram consumes).
HFT_ALWAYS_INLINE std::uint64_t tsc_to_ns_u(std::uint64_t cycles) noexcept {
  return static_cast<std::uint64_t>(tsc_to_ns(cycles) + 0.5);
}

}  // namespace hft
