// HFT-shaped limit order book: a flat array indexed by tick offset.
//
// A textbook book is a std::map<price, level> (or a tree) — O(log n) per update,
// pointer-chasing, cache-hostile, heap-allocating. None of that belongs on a
// market-data hot path. Instead we quantize prices to ticks and store resting
// size in a flat array indexed by (price - base) / tick:
//
//     index   0          1          2     ...                NumTicks-1
//     price   base     base+tick  base+2t ...        base+(NumTicks-1)*tick
//     bid_[]  [ size at each price level on the bid side ]
//     ask_[]  [ size at each price level on the ask side ]
//
//   * Update is O(1): compute the index, add/subtract size. No allocation, no
//     branches that depend on tree shape, no cache misses beyond the one line
//     holding that level.
//   * Top-of-book is O(1): we cache best_bid_idx_ / best_ask_idx_ and maintain
//     them incrementally. An add that improves the touch updates the cached
//     index directly; the only non-O(1) path is when the *current best* level
//     is fully consumed, where we walk to the next non-empty level — almost
//     always the adjacent tick, and bounded by book depth.
//   * Compact: two u32 arrays. With NumTicks = 4096 that is 32 KB total — it
//     lives in L1/L2 and never moves.
//
// The window covers a fixed price band around a reference. Prices outside the
// band are counted and ignored (a production book would re-center; for a
// bounded-volatility simulation we size the band generously). This is the
// engine's own book — deliberately separate from any general-purpose book.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hft/compiler.hpp"
#include "hft/itch_message.hpp"

namespace hft {

template <std::size_t NumTicks>
class BookView {
  static_assert(NumTicks >= 4, "book window too small");

 public:
  // base_price = fixed-point price at index 0; tick = fixed-point tick size.
  BookView(std::int64_t base_price, std::int64_t tick) noexcept
      : base_(base_price), tick_(tick) {}

  // Apply one market-data message. O(1) except when the touched best level is
  // emptied (then a short walk to the next level).
  HFT_ALWAYS_INLINE void apply(const ItchMessage& m) noexcept {
    const int idx = to_index(m.price);
    if (HFT_UNLIKELY(idx < 0 || idx >= static_cast<int>(NumTicks))) {
      ++out_of_range_;
      return;
    }
    const bool is_bid = m.side == static_cast<std::uint8_t>(Side::kBuy);
    std::uint32_t* HFT_RESTRICT arr = is_bid ? bid_.data() : ask_.data();

    switch (static_cast<MsgType>(m.type)) {
      case MsgType::kAdd:
        arr[idx] += m.size;
        on_size_increased(is_bid, idx);
        break;
      case MsgType::kExecute:
      case MsgType::kCancel:
        arr[idx] = (m.size >= arr[idx]) ? 0u : arr[idx] - m.size;
        if (arr[idx] == 0) on_level_emptied(is_bid, idx);
        break;
      case MsgType::kDelete:
        arr[idx] = 0;
        on_level_emptied(is_bid, idx);
        break;
      case MsgType::kTrade:
      case MsgType::kUnknown:
      default:
        break;  // informational / non-book
    }
  }

  // ---- top of book (all O(1)) --------------------------------------------
  bool has_bid() const noexcept { return best_bid_idx_ >= 0; }
  bool has_ask() const noexcept { return best_ask_idx_ < static_cast<int>(NumTicks); }
  bool two_sided() const noexcept { return has_bid() && has_ask(); }

  std::int64_t best_bid_price() const noexcept { return price_at(best_bid_idx_); }
  std::int64_t best_ask_price() const noexcept { return price_at(best_ask_idx_); }
  std::uint32_t best_bid_size() const noexcept {
    return has_bid() ? bid_[static_cast<std::size_t>(best_bid_idx_)] : 0u;
  }
  std::uint32_t best_ask_size() const noexcept {
    return has_ask() ? ask_[static_cast<std::size_t>(best_ask_idx_)] : 0u;
  }

  // Top-of-book imbalance in [-1, +1]: (bid - ask) / (bid + ask). 0 if the book
  // is not two-sided. Computed in floating point here (cold-ish); the strategy
  // uses an integer-only form on the hot path.
  double imbalance() const noexcept {
    if (!two_sided()) return 0.0;
    const double b = best_bid_size();
    const double a = best_ask_size();
    const double denom = b + a;
    return denom > 0.0 ? (b - a) / denom : 0.0;
  }

  // Inspection helper (tests): resting size at an arbitrary price/side.
  std::uint32_t size_at(Side side, std::int64_t price) const noexcept {
    const int idx = to_index(price);
    if (idx < 0 || idx >= static_cast<int>(NumTicks)) return 0;
    return (side == Side::kBuy) ? bid_[static_cast<std::size_t>(idx)]
                                : ask_[static_cast<std::size_t>(idx)];
  }

  std::uint64_t out_of_range() const noexcept { return out_of_range_; }

 private:
  HFT_ALWAYS_INLINE int to_index(std::int64_t price) const noexcept {
    return static_cast<int>((price - base_) / tick_);
  }
  HFT_ALWAYS_INLINE std::int64_t price_at(int idx) const noexcept {
    return base_ + static_cast<std::int64_t>(idx) * tick_;
  }

  HFT_ALWAYS_INLINE void on_size_increased(bool is_bid, int idx) noexcept {
    if (is_bid) {
      if (idx > best_bid_idx_) best_bid_idx_ = idx;  // better (higher) bid
    } else {
      if (idx < best_ask_idx_) best_ask_idx_ = idx;  // better (lower) ask
    }
  }

  // The level at idx just hit zero. Only the *best* level emptying moves the
  // touch; interior levels emptying are irrelevant to top-of-book.
  HFT_NOINLINE void on_level_emptied(bool is_bid, int idx) noexcept {
    if (is_bid) {
      if (idx != best_bid_idx_) return;
      int j = idx - 1;
      while (j >= 0 && bid_[static_cast<std::size_t>(j)] == 0) --j;
      best_bid_idx_ = j;  // -1 if the bid side is now empty
    } else {
      if (idx != best_ask_idx_) return;
      int j = idx + 1;
      while (j < static_cast<int>(NumTicks) && ask_[static_cast<std::size_t>(j)] == 0) ++j;
      best_ask_idx_ = j;  // NumTicks if the ask side is now empty
    }
  }

  std::array<std::uint32_t, NumTicks> bid_{};
  std::array<std::uint32_t, NumTicks> ask_{};
  int best_bid_idx_ = -1;                            // -1 => no bid
  int best_ask_idx_ = static_cast<int>(NumTicks);    // NumTicks => no ask
  std::int64_t base_;
  std::int64_t tick_;
  std::uint64_t out_of_range_ = 0;
};

}  // namespace hft
