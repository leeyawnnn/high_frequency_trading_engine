// Phase 1: latency histogram. Records 1M samples and verifies percentiles
// against an exact sorted reference, plus exactness on the linear low range.
#include "test_harness.hpp"
#include "hft/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

// Exact nearest-rank percentile over a sorted vector, using the SAME rank
// convention as Histogram::percentile so we isolate bucket error from any
// off-by-one in rank selection.
std::uint64_t reference_percentile(const std::vector<std::uint64_t>& sorted,
                                   double p) {
  const std::uint64_t n = sorted.size();
  std::uint64_t want = static_cast<std::uint64_t>(
      (p / 100.0) * static_cast<double>(n) + 0.5);
  if (want == 0) want = 1;
  if (want > n) want = n;
  return sorted[want - 1];
}

}  // namespace

HFT_TEST(low_range_is_exact) {
  // In the first octave, sub-buckets have width 1 ns → exact.
  hft::Histogram<> h;
  for (std::uint64_t v = 0; v < 64; ++v) h.record(v);
  CHECK_EQ(h.count(), 64u);
  CHECK_EQ(h.min(), 0u);
  CHECK_EQ(h.max(), 63u);
  // Every distinct value lands in its own bucket here.
  CHECK_EQ(h.percentile(0.0), 0u);
  CHECK_EQ(h.percentile(100.0), 63u);
}

HFT_TEST(single_value) {
  hft::Histogram<> h;
  h.record_n(500, 1000);
  CHECK_EQ(h.count(), 1000u);
  // 500 ns sits in a bucket whose upper bound is within 3.1% of 500.
  CHECK_NEAR(h.percentile(50.0), 500.0, 0.04);
  CHECK_NEAR(h.mean(), 500.0, 1e-9);
}

HFT_TEST(percentiles_match_reference_1M) {
  constexpr int kN = 1'000'000;
  hft::Histogram<> h;
  std::vector<std::uint64_t> ref;
  ref.reserve(kN);

  // Deterministic lognormal-ish latency distribution: a body around a few
  // hundred ns plus a heavy tail out to tens of microseconds.
  std::mt19937_64 rng(0xC0FFEEULL);
  std::lognormal_distribution<double> body(std::log(300.0), 0.6);
  std::uniform_real_distribution<double> tail_roll(0.0, 1.0);
  std::lognormal_distribution<double> tail(std::log(8000.0), 0.7);

  std::uint64_t exact_sum = 0;
  for (int i = 0; i < kN; ++i) {
    double sample = (tail_roll(rng) < 0.01) ? tail(rng) : body(rng);
    std::uint64_t ns = static_cast<std::uint64_t>(std::llround(sample));
    if (ns < 1) ns = 1;
    if (ns > 1'000'000'000ULL) ns = 1'000'000'000ULL;
    h.record(ns);
    ref.push_back(ns);
    exact_sum += ns;
  }

  std::sort(ref.begin(), ref.end());
  CHECK_EQ(h.count(), static_cast<std::uint64_t>(kN));
  CHECK_EQ(h.min(), ref.front());
  CHECK_EQ(h.max(), ref.back());

  const double exact_mean =
      static_cast<double>(exact_sum) / static_cast<double>(kN);
  CHECK_NEAR(h.mean(), exact_mean, 1e-9);

  // PrecisionBits=5 → <=3.125% bucket error. Allow 4% to cover rounding.
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
    const std::uint64_t got = h.percentile(p);
    const std::uint64_t want = reference_percentile(ref, p);
    const double rel =
        std::fabs(static_cast<double>(got) - static_cast<double>(want)) /
        static_cast<double>(want);
    if (rel > 0.04) {
      std::fprintf(stderr,
                   "    p%.2f: hist=%llu ref=%llu rel=%.4f\n", p,
                   static_cast<unsigned long long>(got),
                   static_cast<unsigned long long>(want), rel);
    }
    CHECK(rel <= 0.04);
  }

  // Percentiles must be monotonic non-decreasing.
  std::uint64_t prev = 0;
  for (double p = 1.0; p <= 100.0; p += 1.0) {
    const std::uint64_t v = h.percentile(p);
    CHECK(v >= prev);
    prev = v;
  }
}

HFT_TEST(merge_is_additive) {
  hft::Histogram<> a, b;
  for (std::uint64_t v = 100; v < 1100; ++v) a.record(v);
  for (std::uint64_t v = 100; v < 1100; ++v) b.record(v);
  hft::Histogram<> merged;
  merged.add(a);
  merged.add(b);
  CHECK_EQ(merged.count(), 2000u);
  CHECK_EQ(merged.min(), 100u);
  CHECK_EQ(merged.max(), 1099u);
  // Same distribution doubled → percentiles unchanged vs a single copy.
  CHECK_EQ(merged.percentile(50.0), a.percentile(50.0));
}

HFT_TEST(overflow_clamps) {
  hft::Histogram<1'000'000ULL, 5> h;  // cap at 1ms
  h.record(5'000'000);                // 5ms, over cap
  CHECK_EQ(h.count(), 1u);
  CHECK(h.percentile(100.0) <= 1'000'000ULL + (1'000'000ULL >> 5));
}

HFT_TEST_MAIN()
