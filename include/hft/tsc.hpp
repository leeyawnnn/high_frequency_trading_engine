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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
#define HFT_TSC_X86 1
#else
#define HFT_TSC_X86 0
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
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
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

// Which hardware counter the measurements actually came from. This travels
// into every output artifact: a latency table means nothing without it,
// because the floor of what can be measured is a property of the counter and
// not of the code being measured.
enum class ClockSource : std::uint8_t {
  kX86InvariantTsc,     // rdtsc / rdtscp
  kArmCntvct,           // cntvct_el0, ticking at cntfrq_el0
  kSteadyClockFallback  // neither; std::chrono, and far coarser
};

[[nodiscard]] inline const char* to_string(ClockSource s) noexcept {
  switch (s) {
    case ClockSource::kX86InvariantTsc:
      return "x86-invariant-tsc";
    case ClockSource::kArmCntvct:
      return "arm-cntvct-el0";
    case ClockSource::kSteadyClockFallback:
      return "steady-clock-fallback";
  }
  return "unknown";
}

struct TscCalibration {
  double ns_per_cycle = 1.0;   // multiply a cycle-delta by this to get ns
  double cycles_per_ns = 1.0;  // inverse
  std::uint64_t hz = 0;        // measured counter frequency
  // Frequency the hardware declares for itself, where it declares one at all
  // (AArch64 cntfrq_el0). 0 when unavailable. Cross-checking the measured
  // frequency against this is how a bad calibration gets caught.
  std::uint64_t nominal_hz = 0;
  // Period of a single counter tick. This is the resolution floor: no
  // measurement can be finer, and any reported value below a couple of ticks
  // is quantisation, not signal.
  double resolution_ns = 1.0;
  ClockSource source = ClockSource::kSteadyClockFallback;
};

// Read the counter frequency the hardware advertises, or 0 if it advertises
// none. AArch64 exposes cntfrq_el0; x86 has no architectural equivalent, so
// the measured calibration is the only source there.
[[nodiscard]] inline std::uint64_t hardware_counter_hz() noexcept {
#if defined(__aarch64__)
  std::uint64_t v = 0;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
#else
  return 0;
#endif
}

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

    const double ns = std::chrono::duration<double, std::nano>(w1 - w0).count();
    const double cycles = static_cast<double>(c1 - c0);
    ns_per_cycle[static_cast<std::size_t>(i)] = (cycles > 0.0) ? (ns / cycles) : 1.0;
  }

  std::sort(ns_per_cycle.begin(), ns_per_cycle.end());
  const double median = ns_per_cycle[kRounds / 2];  // robust to outliers

  TscCalibration cal;
  cal.ns_per_cycle = median;
  cal.cycles_per_ns = (median > 0.0) ? (1.0 / median) : 1.0;
  cal.hz = static_cast<std::uint64_t>((median > 0.0) ? (1e9 / median) : 0.0);
  cal.nominal_hz = hardware_counter_hz();

#if HFT_TSC_X86
  cal.source = ClockSource::kX86InvariantTsc;
#elif defined(__aarch64__)
  cal.source = ClockSource::kArmCntvct;
#else
  cal.source = ClockSource::kSteadyClockFallback;
#endif

  // Prefer the hardware's own figure for the tick period when it publishes
  // one: it is exact, where the measured value carries the scheduling noise of
  // the calibration window. They should agree closely, and a large discrepancy
  // means the calibration is not to be trusted.
  cal.resolution_ns = (cal.nominal_hz > 0) ? (1e9 / static_cast<double>(cal.nominal_hz)) : median;
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
inline void tsc_calibrate() {
  (void)tsc_calibration();
}

// ---------------------------------------------------------------------------
// Conversion (cold path: reporting only).
// ---------------------------------------------------------------------------

// Convert a cycle *delta* to nanoseconds.
HFT_ALWAYS_INLINE double tsc_to_ns(std::uint64_t cycles) noexcept {
  return static_cast<double>(cycles) * tsc_calibration().ns_per_cycle;
}

// Same, rounded to whole nanoseconds (what the histogram consumes).
HFT_ALWAYS_INLINE std::uint64_t tsc_to_ns_u(std::uint64_t cycles) noexcept {
  return static_cast<std::uint64_t>(std::llround(tsc_to_ns(cycles)));
}

// ---------------------------------------------------------------------------
// Resolution honesty.
//
// A counter that ticks every 41.67 ns cannot measure a 20 ns span. It reports
// 0 or 1 tick, and averaging many such samples still yields a multiple of the
// tick period. Printing "p50 = 0 ns" from such a counter states that an
// operation took no time, when what was actually observed is that it finished
// inside one tick. These helpers make that distinction explicit rather than
// leaving it for the reader to infer.
// ---------------------------------------------------------------------------

// Below this many tick periods, a figure is quantisation rather than signal.
// Two ticks is the smallest span for which a non-zero reading is unambiguous.
inline constexpr double kResolutionFloorTicks = 2.0;

[[nodiscard]] inline double resolution_floor_ns() noexcept {
  return kResolutionFloorTicks * tsc_calibration().resolution_ns;
}

[[nodiscard]] inline bool below_resolution(std::uint64_t ns) noexcept {
  return static_cast<double>(ns) < resolution_floor_ns();
}

// Render a nanosecond figure for a report or a CSV cell. Values the clock
// cannot resolve render as "<R" (R being the floor) instead of as a number
// that would be read as a measurement.
inline void format_ns(char* buf, std::size_t cap, std::uint64_t ns) noexcept {
  if (below_resolution(ns)) {
    std::snprintf(buf, cap, "<%.0f", resolution_floor_ns());
  } else {
    std::snprintf(buf, cap, "%llu", static_cast<unsigned long long>(ns));
  }
}

// Startup self-check. Prints what the measurement is actually standing on, so
// that a report carries its own caveats instead of relying on the reader to
// know the platform. Written to the report header and mirrored into the CSVs.
inline void print_clock_report(std::FILE* out) {
  const TscCalibration& c = tsc_calibration();
  std::fprintf(out, "clock source      : %s\n", to_string(c.source));
  std::fprintf(out, "measured frequency: %.6f MHz\n", static_cast<double>(c.hz) / 1e6);
  if (c.nominal_hz > 0) {
    const double nominal_mhz = static_cast<double>(c.nominal_hz) / 1e6;
    const double drift_pct = 100.0 *
                             (static_cast<double>(c.hz) - static_cast<double>(c.nominal_hz)) /
                             static_cast<double>(c.nominal_hz);
    std::fprintf(out, "nominal frequency : %.6f MHz (hardware cntfrq_el0)\n", nominal_mhz);
    std::fprintf(out, "calibration drift : %+.3f%%\n", drift_pct);
  } else {
    std::fprintf(out, "nominal frequency : not advertised by this architecture\n");
  }
  std::fprintf(out, "tick period       : %.3f ns\n", c.resolution_ns);
  std::fprintf(out, "reporting floor   : %.0f ns (%.0f ticks; finer values print as \"<floor\")\n",
               resolution_floor_ns(), kResolutionFloorTicks);
}

}  // namespace hft
