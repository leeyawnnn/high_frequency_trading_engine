// Order gateway stage: order SPSC -> risk gate -> wire, plus fill intake.
//
// Pinned to its own core. Responsibilities:
//   * Pop OrderRequests the strategy produced.
//   * Run the pre-trade RiskGate (the last line of defense before the wire).
//   * Track the order in a fixed-size, direct-mapped in-flight table (index =
//     order_id & mask) — NO std::map, NO allocation.
//   * Serialize (the OrderRequest *is* the wire format) and send over UDP.
//   * Receive fills, match them to in-flight orders, update the risk position,
//     forward them to the strategy, and close the end-to-end latency loop.
//
// Latencies recorded:
//   * gateway_latency_ : order dequeue -> on the wire (includes the risk check).
//   * e2e_inproc_      : feed-arrival TSC (carried in md_timestamp) -> wire.
//                        This is the headline "data in -> order out" number.
//   * round_trip_      : feed-arrival TSC -> fill received back.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hft/compiler.hpp"
#include "hft/histogram.hpp"
#include "hft/net.hpp"
#include "hft/order_msg.hpp"
#include "hft/risk_gate.hpp"
#include "hft/tsc.hpp"

namespace hft {

template <typename OrderQueue, typename FillQueue, std::size_t InFlightCapacity = 4096>
class OrderGateway {
  static_assert((InFlightCapacity & (InFlightCapacity - 1)) == 0,
                "InFlightCapacity must be a power of two");
  static constexpr std::uint64_t kMask = InFlightCapacity - 1;

  struct InFlight {
    std::uint64_t order_id = 0;
    std::uint64_t send_tsc = 0;
    std::uint64_t md_tsc = 0;  // carried feed-arrival stamp
    bool active = false;
  };

 public:
  OrderGateway(OrderQueue& orders, FillQueue& fills_out, const Endpoint& order_dst,
               std::uint16_t fill_port, const RiskLimits& limits,
               const std::atomic<bool>& kill, const char* fill_bind = "0.0.0.0")
      : orders_(&orders),
        fills_out_(&fills_out),
        order_dst_(order_dst),
        fill_rx_(UdpSocket::bound(fill_port, fill_bind)),
        risk_(limits, kill) {
    fill_rx_.set_nonblocking(true);
    fill_rx_.set_recv_buffer(1 << 20);
    order_tx_.set_send_buffer(1 << 20);
  }

  std::uint16_t fill_local_port() const { return fill_rx_.local_port(); }

  // Set/replace the order destination (used to wire ephemeral ports after both
  // ends are constructed). Call before run().
  void set_order_destination(const Endpoint& dst) noexcept { order_dst_ = dst; }

  // Drain inbound fills, then send at most one outbound order. Returns true if
  // an order was processed (sent or risk-rejected).
  HFT_ALWAYS_INLINE bool poll_once() noexcept {
    drain_fills();

    OrderRequest o{};
    if (!orders_->pop(o)) return false;

    const std::uint64_t t0 = tsc_now();
    const RiskResult rr = risk_.check(o);
    if (HFT_UNLIKELY(rr != RiskResult::kAccept)) {
      ++risk_rejected_;
      return true;  // dropped at the gate; never hits the wire
    }

    const std::uint64_t idx = o.order_id & kMask;
    const std::uint64_t send_t = tsc_now();
    inflight_[idx] = InFlight{o.order_id, send_t, o.md_timestamp, true};

    order_tx_.send_to(order_dst_, &o, sizeof(o));  // OrderRequest is the wire msg

    gateway_latency_.record(tsc_to_ns_u(send_t - t0));
    if (o.md_timestamp != 0)
      e2e_inproc_.record(tsc_to_ns_u(send_t - o.md_timestamp));
    ++sent_;
    return true;
  }

  void run(const std::atomic<bool>& running, int core);

  // ---- inspection ---------------------------------------------------------
  std::uint64_t sent() const noexcept { return sent_; }
  std::uint64_t risk_rejected() const noexcept { return risk_rejected_; }
  std::uint64_t fills_matched() const noexcept { return fills_matched_; }
  std::uint64_t fills_unmatched() const noexcept { return fills_unmatched_; }
  const Histogram<>& gateway_latency() const noexcept { return gateway_latency_; }
  const Histogram<>& e2e_latency() const noexcept { return e2e_inproc_; }
  const Histogram<>& round_trip_latency() const noexcept { return round_trip_; }
  const RiskGate& risk() const noexcept { return risk_; }

 private:
  HFT_ALWAYS_INLINE void drain_fills() noexcept {
    ExecReport rep{};
    for (int d = 0; d < 32; ++d) {
      const long n = fill_rx_.try_recv(&rep, sizeof(rep));
      if (n != static_cast<long>(kExecSize)) break;
      handle_fill(rep);
    }
  }

  HFT_ALWAYS_INLINE void handle_fill(const ExecReport& rep) noexcept {
    const std::uint64_t idx = rep.order_id & kMask;
    InFlight& slot = inflight_[idx];
    if (HFT_LIKELY(slot.active && slot.order_id == rep.order_id)) {
      slot.active = false;
      ++fills_matched_;
      risk_.on_fill(rep);  // authoritative position update
      if (rep.md_timestamp != 0)
        round_trip_.record(tsc_to_ns_u(tsc_now() - rep.md_timestamp));
      while (HFT_UNLIKELY(!fills_out_->push(rep))) { /* to strategy */ }
    } else {
      ++fills_unmatched_;  // duplicate, stale, or table collision
    }
  }

  OrderQueue* orders_;
  FillQueue* fills_out_;
  Endpoint order_dst_;
  UdpSocket order_tx_;
  UdpSocket fill_rx_;
  RiskGate risk_;

  std::array<InFlight, InFlightCapacity> inflight_{};
  Histogram<> gateway_latency_;  // dequeue -> wire
  Histogram<> e2e_inproc_;       // feed arrival -> wire (in-process)
  Histogram<> round_trip_;       // feed arrival -> fill received
  std::uint64_t sent_ = 0;
  std::uint64_t risk_rejected_ = 0;
  std::uint64_t fills_matched_ = 0;
  std::uint64_t fills_unmatched_ = 0;
};

}  // namespace hft

#include "hft/affinity.hpp"

namespace hft {

template <typename OrderQueue, typename FillQueue, std::size_t InFlightCapacity>
void OrderGateway<OrderQueue, FillQueue, InFlightCapacity>::run(
    const std::atomic<bool>& running, int core) {
  pin_current_thread(core);
  set_current_thread_name("hft-gw");
  while (running.load(std::memory_order_relaxed)) {
    poll_once();
  }
}

}  // namespace hft
