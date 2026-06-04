// Pre-trade risk gate: the last line of defense before an order leaves the box.
//
// Every check is O(1) and the whole thing is a tiny inline function returning
// accept/deny. Four checks, ordered cheapest-and-most-critical first so the
// common accept path does the least work and a kill-switch trip short-circuits
// immediately:
//
//   1. Kill switch   — a global atomic flag; when set, nothing gets out.
//   2. Order size    — reject zero-size and anything over the per-order cap.
//   3. Position limit— reject if the order would push net position out of band.
//   4. Rate limit    — token bucket, so a runaway strategy can't flood the wire.
//
// The rate limiter is a GCRA (generic cell rate algorithm) token bucket
// implemented in pure TSC cycles — integer-only, no floating point on the hot
// path, no division per check (the per-token interval is precomputed once).
//
// In a real firm checks 2-4 live partly in an FPGA on the NIC; here they are
// software, but the shape — fixed-cost, allocation-free, integer — is the same.
#pragma once

#include <atomic>
#include <cstdint>

#include "hft/compiler.hpp"
#include "hft/itch_message.hpp"  // Side
#include "hft/order_msg.hpp"
#include "hft/tsc.hpp"

namespace hft {

enum class RiskResult : std::uint8_t {
  kAccept = 0,
  kKillSwitch,
  kOrderSize,
  kPositionLimit,
  kRateLimit,
};

struct RiskLimits {
  std::int64_t max_position;       // absolute net-position bound (shares)
  std::uint32_t max_order_size;    // per-order cap (shares)
  double max_orders_per_sec;       // token-bucket refill rate (0 = unlimited)
  std::uint32_t burst;             // token-bucket depth (orders allowed back-to-back)
};

class RiskGate {
 public:
  RiskGate(const RiskLimits& lim, const std::atomic<bool>& kill) noexcept
      : kill_(&kill),
        max_position_(lim.max_position),
        max_order_size_(lim.max_order_size) {
    const double hz = tsc_calibration().cycles_per_ns * 1e9;
    cycles_per_token_ =
        (lim.max_orders_per_sec > 0.0)
            ? static_cast<std::uint64_t>(hz / lim.max_orders_per_sec)
            : 0;
    tau_ = cycles_per_token_ * (lim.burst ? lim.burst : 1);
  }

  // The hot-path check. Allocation-free, integer-only, O(1).
  HFT_ALWAYS_INLINE RiskResult check(const OrderRequest& o) noexcept {
    // 1. Kill switch.
    if (HFT_UNLIKELY(kill_->load(std::memory_order_relaxed))) {
      ++rej_kill_;
      return RiskResult::kKillSwitch;
    }
    // 2. Order size.
    if (HFT_UNLIKELY(o.size == 0 || o.size > max_order_size_)) {
      ++rej_size_;
      return RiskResult::kOrderSize;
    }
    // 3. Position limit (projected net position stays within [-max, +max]).
    const std::int64_t signed_sz =
        (o.side == static_cast<std::uint8_t>(Side::kBuy))
            ? static_cast<std::int64_t>(o.size)
            : -static_cast<std::int64_t>(o.size);
    const std::int64_t projected = position_ + signed_sz;
    if (HFT_UNLIKELY(projected > max_position_ || projected < -max_position_)) {
      ++rej_pos_;
      return RiskResult::kPositionLimit;
    }
    // 4. Rate limit (GCRA token bucket in TSC cycles). Checked last so a token
    //    is only consumed by an order that would otherwise pass.
    if (cycles_per_token_) {
      const std::uint64_t now = tsc_now();
      const std::uint64_t earliest = (tat_ > tau_) ? tat_ - tau_ : 0;
      if (HFT_UNLIKELY(now < earliest)) {
        ++rej_rate_;
        return RiskResult::kRateLimit;
      }
      tat_ = (now > tat_ ? now : tat_) + cycles_per_token_;
    }
    ++accepted_;
    return RiskResult::kAccept;
  }

  HFT_ALWAYS_INLINE bool allow(const OrderRequest& o) noexcept {
    return check(o) == RiskResult::kAccept;
  }

  // Keep the gate's position view in sync with reality.
  void on_fill(const ExecReport& r) noexcept {
    if (r.side == static_cast<std::uint8_t>(Side::kBuy))
      position_ += static_cast<std::int64_t>(r.fill_size);
    else
      position_ -= static_cast<std::int64_t>(r.fill_size);
  }

  void set_position(std::int64_t p) noexcept { position_ = p; }

  // ---- inspection ---------------------------------------------------------
  std::int64_t position() const noexcept { return position_; }
  std::uint64_t accepted() const noexcept { return accepted_; }
  std::uint64_t rejected_kill() const noexcept { return rej_kill_; }
  std::uint64_t rejected_size() const noexcept { return rej_size_; }
  std::uint64_t rejected_position() const noexcept { return rej_pos_; }
  std::uint64_t rejected_rate() const noexcept { return rej_rate_; }

 private:
  const std::atomic<bool>* kill_;
  std::int64_t position_ = 0;
  std::int64_t max_position_;
  std::uint32_t max_order_size_;

  // GCRA state (all in TSC cycles).
  std::uint64_t cycles_per_token_ = 0;  // emission interval T
  std::uint64_t tau_ = 0;               // burst tolerance
  std::uint64_t tat_ = 0;               // theoretical arrival time

  std::uint64_t accepted_ = 0;
  std::uint64_t rej_kill_ = 0;
  std::uint64_t rej_size_ = 0;
  std::uint64_t rej_pos_ = 0;
  std::uint64_t rej_rate_ = 0;
};

}  // namespace hft
