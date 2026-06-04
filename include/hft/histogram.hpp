// Fixed-size latency histogram with bounded relative error.
//
// Design: HdrHistogram-style log-linear bucketing. The value range is split
// into power-of-two "octaves"; each octave holds 2^PrecisionBits linear
// sub-buckets. This guarantees a relative error <= 2^-PrecisionBits across the
// entire range (1 ns .. HighestTrackableNs) while using a small, fixed array —
// so record() is a couple of bit ops plus one array increment, with NO
// allocation, NO branches that depend on data in a hard-to-predict way, and
// NO floating point on the recording path.
//
// The counts array is sized at compile time, so a Histogram can live as a
// member of a hot-path struct and be recorded into from a pinned thread with
// zero heap traffic.
#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include "hft/compiler.hpp"

namespace hft {

// PrecisionBits = significant bits below the leading bit → relative error
// 2^-PrecisionBits. Default 5 → ~3.1% bucket error.
template <std::uint64_t HighestTrackableNs = 1'000'000'000ULL,  // 1 second
          unsigned PrecisionBits = 5>
class Histogram {
  static_assert(PrecisionBits >= 1 && PrecisionBits <= 16, "unreasonable precision");

  static constexpr unsigned kSubBucketHalfCountMagnitude = PrecisionBits;
  static constexpr std::uint32_t kSubBucketCount = 1u << (PrecisionBits + 1);
  static constexpr std::uint32_t kSubBucketHalfCount = kSubBucketCount >> 1;
  static constexpr std::uint64_t kSubBucketMask = kSubBucketCount - 1;
  // 64 - (subBucketHalfCountMagnitude + 1), unit magnitude = 0 (1 ns resolution)
  static constexpr int kLeadingZeroCountBase =
      64 - static_cast<int>(kSubBucketHalfCountMagnitude) - 1;

  static constexpr int buckets_needed(std::uint64_t value) {
    std::uint64_t smallest_untrackable = kSubBucketCount;  // unit magnitude 0
    int needed = 1;
    while (smallest_untrackable <= value) {
      if (smallest_untrackable > (std::numeric_limits<std::uint64_t>::max() >> 1))
        return needed + 1;  // would overflow on next shift
      smallest_untrackable <<= 1;
      ++needed;
    }
    return needed;
  }

  static constexpr std::size_t kBucketCount = buckets_needed(HighestTrackableNs);
  static constexpr std::size_t kCountsLen =
      (kBucketCount + 1) * kSubBucketHalfCount;

 public:
  Histogram() = default;

  // ---- recording (hot path) ----------------------------------------------
  HFT_ALWAYS_INLINE void record(std::uint64_t value) noexcept {
    if (HFT_UNLIKELY(value > HighestTrackableNs)) value = HighestTrackableNs;
    const std::size_t idx = counts_index(value);
    ++counts_[idx];
    ++total_count_;
    sum_ns_ += value;
    if (value > max_) max_ = value;
    if (value < min_) min_ = value;
  }

  void record_n(std::uint64_t value, std::uint64_t n) noexcept {
    if (HFT_UNLIKELY(value > HighestTrackableNs)) value = HighestTrackableNs;
    counts_[counts_index(value)] += n;
    total_count_ += n;
    sum_ns_ += value * n;
    if (value > max_) max_ = value;
    if (value < min_) min_ = value;
  }

  // ---- queries (cold path) ------------------------------------------------
  std::uint64_t count() const noexcept { return total_count_; }
  std::uint64_t min() const noexcept { return total_count_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return total_count_ ? static_cast<double>(sum_ns_) /
                              static_cast<double>(total_count_)
                        : 0.0;
  }

  // Value at the given percentile in [0,100]. Returns the highest-equivalent
  // value of the containing bucket (HdrHistogram convention), so it is an
  // upper bound on the true value within the bucket's relative error.
  std::uint64_t percentile(double p) const noexcept {
    if (total_count_ == 0) return 0;
    if (p < 0.0) p = 0.0;
    if (p > 100.0) p = 100.0;
    // Rank of the requested percentile (1-based count threshold).
    const std::uint64_t target =
        static_cast<std::uint64_t>((p / 100.0) * static_cast<double>(total_count_) + 0.5);
    const std::uint64_t want = target == 0 ? 1 : target;

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kCountsLen; ++i) {
      cumulative += counts_[i];
      if (cumulative >= want) return highest_equivalent_value(i);
    }
    return max_;
  }

  // Merge another histogram (e.g. combine per-thread captures).
  template <std::uint64_t H2, unsigned P2>
  void add(const Histogram<H2, P2>& other) noexcept {
    static_assert(H2 == HighestTrackableNs && P2 == PrecisionBits,
                  "can only merge histograms with identical parameters");
    for (std::size_t i = 0; i < kCountsLen; ++i) counts_[i] += other.raw_count(i);
    total_count_ += other.count();
    sum_ns_ += other.sum();
    if (other.max() > max_) max_ = other.max();
    if (other.count() && other.min() < min_) min_ = other.min();
  }

  void reset() noexcept {
    counts_.fill(0);
    total_count_ = 0;
    sum_ns_ = 0;
    max_ = 0;
    min_ = std::numeric_limits<std::uint64_t>::max();
  }

  // Accessors used by add() and by tools that dump raw buckets.
  std::uint64_t raw_count(std::size_t i) const noexcept { return counts_[i]; }
  std::uint64_t sum() const noexcept { return sum_ns_; }
  static constexpr std::size_t counts_len() noexcept { return kCountsLen; }
  // Inclusive upper bound (ns) of bucket i — for dumping bucket edges.
  std::uint64_t bucket_upper_bound(std::size_t i) const noexcept {
    return highest_equivalent_value(i);
  }

 private:
  // ---- HdrHistogram index math -------------------------------------------
  static HFT_ALWAYS_INLINE int bucket_index(std::uint64_t value) noexcept {
    // __builtin_clzll is UB for 0, but (value | mask) is always >= mask >= 1.
    return kLeadingZeroCountBase -
           __builtin_clzll(value | kSubBucketMask);
  }
  static HFT_ALWAYS_INLINE std::uint32_t sub_bucket_index(std::uint64_t value,
                                                          int bidx) noexcept {
    return static_cast<std::uint32_t>(value >> bidx);  // unit magnitude 0
  }
  static HFT_ALWAYS_INLINE std::size_t counts_index(std::uint64_t value) noexcept {
    const int bidx = bucket_index(value);
    const std::uint32_t sidx = sub_bucket_index(value, bidx);
    // Signed arithmetic: in the first octave (bidx==0) sidx < subBucketHalfCount
    // so the offset is negative — the sum stays correct, but doing it unsigned
    // would underflow into a gigantic index. (Classic HdrHistogram detail.)
    const std::int64_t bucket_base =
        static_cast<std::int64_t>(bidx + 1) << kSubBucketHalfCountMagnitude;
    const std::int64_t offset =
        static_cast<std::int64_t>(sidx) - static_cast<std::int64_t>(kSubBucketHalfCount);
    return static_cast<std::size_t>(bucket_base + offset);
  }

  // Inverse: representative (highest-equivalent) value of a counts index.
  static std::uint64_t highest_equivalent_value(std::size_t index) noexcept {
    // Recover (bucketIndex, subBucketIndex) from the flat index.
    int bidx = static_cast<int>(index >> kSubBucketHalfCountMagnitude) - 1;
    std::uint32_t sidx =
        static_cast<std::uint32_t>(index & (kSubBucketHalfCount - 1)) +
        kSubBucketHalfCount;
    if (bidx < 0) {  // first (linear) bucket: indices 0..kSubBucketHalfCount-1
      bidx = 0;
      sidx = static_cast<std::uint32_t>(index);
    }
    const std::uint64_t lower = static_cast<std::uint64_t>(sidx) << bidx;
    const std::uint64_t bucket_unit = std::uint64_t{1} << bidx;
    return lower + bucket_unit - 1;  // inclusive top of this bucket
  }

  std::array<std::uint64_t, kCountsLen> counts_{};
  std::uint64_t total_count_ = 0;
  std::uint64_t sum_ns_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace hft
