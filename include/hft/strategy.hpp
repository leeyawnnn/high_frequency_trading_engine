// Book-imbalance strategy + the thread runner that drives it.
//
// Signal: at top of book, imbalance = (bid_size - ask_size) / (bid_size +
// ask_size) in [-1, +1].
//   imbalance >  +threshold  and flat  -> BUY  at the bid
//   imbalance <  -threshold  and flat  -> SELL at the ask
// (and, as a natural superset that keeps the position bounded and order flow
//  alive for measurement, the opposite signal flattens an existing position.)
//
// Hot-path discipline:
//   * No floating point and no division on the decision path. The comparison
//     imbalance > T is evaluated as the integer cross-product
//     (bid - ask) * THR_DEN  >  THR_NUM * (bid + ask), which is exact and uses
//     only i64 multiplies the branch predictor never sees mispredict.
//   * The signal flags are computed with arithmetic; the only data-dependent
//     branch is the rare, well-predicted "emit an order" path. The common case
//     (no signal / order in flight) falls straight through.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hft/book_view.hpp"
#include "hft/compiler.hpp"
#include "hft/feed_handler.hpp"   // MdEvent
#include "hft/histogram.hpp"
#include "hft/itch_message.hpp"
#include "hft/order_msg.hpp"
#include "hft/tsc.hpp"

namespace hft {

struct StrategyParams {
  std::int64_t base_price;        // book window base (fixed point)
  std::int64_t tick;              // tick size (fixed point)
  double threshold;               // imbalance trigger in (0, 1)
  std::uint32_t order_size;       // shares per order
};

template <std::size_t NumTicks>
class ImbalanceStrategy {
 public:
  // Fixed-point denominator for the threshold (avoids FP on the hot path).
  static constexpr std::int64_t kThrDen = 1 << 14;  // 16384

  explicit ImbalanceStrategy(const StrategyParams& p) noexcept
      : book_(p.base_price, p.tick),
        order_size_(p.order_size),
        thr_num_(static_cast<std::int64_t>(p.threshold * static_cast<double>(kThrDen) + 0.5)) {}

  // Process one market-data event. `emit` is invoked at most once, with the
  // OrderRequest to send. Allocation-free; integer-only decision.
  template <typename Emit>
  HFT_ALWAYS_INLINE void on_md(const MdEvent& ev, Emit&& emit) noexcept {
    ++md_seen_;
    book_.apply(ev.msg);
    if (HFT_UNLIKELY(!book_.two_sided())) return;

    const std::int64_t bid = book_.best_bid_size();
    const std::int64_t ask = book_.best_ask_size();
    const std::int64_t diff = bid - ask;
    const std::int64_t sum = bid + ask;

    // imbalance > +T  <=>  diff*DEN >  NUM*sum   (sum > 0 always here)
    // imbalance < -T  <=> -diff*DEN >  NUM*sum
    const std::int64_t lhs = diff * kThrDen;
    const std::int64_t rhs = thr_num_ * sum;
    const bool buy = lhs > rhs;
    const bool sell = -lhs > rhs;

    // Branch-light gating: buy if flat-or-short, sell if flat-or-long. buy and
    // sell are mutually exclusive, so at most one fires.
    const bool want_buy = buy & (position_ <= 0);
    const bool want_sell = sell & (position_ >= 0);

    if (HFT_LIKELY(in_flight_ | !(want_buy | want_sell))) return;

    // --- rare path: emit a single order ---
    const bool is_buy = want_buy;
    OrderRequest o{};
    o.order_id = ++last_order_id_;
    o.md_timestamp = ev.arrival_tsc;            // carry the e2e latency tag
    o.symbol = ev.msg.symbol;
    o.size = order_size_;
    o.side = static_cast<std::uint8_t>(is_buy ? Side::kBuy : Side::kSell);
    o.action = static_cast<std::uint8_t>(OrderAction::kNew);
    o.price = is_buy ? book_.best_bid_price() : book_.best_ask_price();

    in_flight_ = true;
    ++orders_sent_;
    emit(static_cast<const OrderRequest&>(o));
  }

  // A fill came back: update position and release the in-flight latch.
  void on_fill(const ExecReport& r) noexcept {
    const std::int64_t qty = r.fill_size;
    if (r.side == static_cast<std::uint8_t>(Side::kBuy))
      position_ += qty;
    else
      position_ -= qty;
    in_flight_ = false;
    ++fills_seen_;
  }

  // ---- inspection ---------------------------------------------------------
  std::int64_t position() const noexcept { return position_; }
  bool in_flight() const noexcept { return in_flight_; }
  std::uint64_t orders_sent() const noexcept { return orders_sent_; }
  std::uint64_t fills_seen() const noexcept { return fills_seen_; }
  std::uint64_t md_seen() const noexcept { return md_seen_; }
  const BookView<NumTicks>& book() const noexcept { return book_; }

 private:
  BookView<NumTicks> book_;
  std::int64_t position_ = 0;       // net shares, in {-order_size, 0, +order_size}
  std::uint64_t last_order_id_ = 0;
  std::uint64_t orders_sent_ = 0;
  std::uint64_t fills_seen_ = 0;
  std::uint64_t md_seen_ = 0;
  std::uint32_t order_size_;
  std::int64_t thr_num_;
  bool in_flight_ = false;
};

// ---------------------------------------------------------------------------
// Thread runner: feed SPSC -> strategy -> order SPSC, draining fills in between.
// FillQ may be null (e.g., a standalone strategy benchmark wiring fills itself).
// ---------------------------------------------------------------------------
template <std::size_t NumTicks, typename FeedQ, typename OrderQ, typename FillQ>
class StrategyRunner {
 public:
  StrategyRunner(const StrategyParams& p, FeedQ& feed, OrderQ& orders,
                 FillQ* fills = nullptr) noexcept
      : strat_(p), feed_(&feed), orders_(&orders), fills_(fills) {}

  // Process pending fills then at most one market-data event. Returns true if a
  // market-data event was processed. Exposed for thread-free testing.
  HFT_ALWAYS_INLINE bool poll_once() noexcept {
    if (fills_) {
      ExecReport r{};
      for (int d = 0; d < 16 && fills_->pop(r); ++d) strat_.on_fill(r);
    }
    MdEvent ev{};
    if (!feed_->pop(ev)) return false;

    const std::uint64_t t0 = tsc_now();
    strat_.on_md(ev, [&](const OrderRequest& o) {
      while (HFT_UNLIKELY(!orders_->push(o))) { /* order queue backpressure */ }
      ++orders_pushed_;
    });
    latency_.record(tsc_to_ns_u(tsc_now() - t0));
    ++processed_;
    return true;
  }

  void run(const std::atomic<bool>& running, int core);

  ImbalanceStrategy<NumTicks>& strategy() noexcept { return strat_; }
  const ImbalanceStrategy<NumTicks>& strategy() const noexcept { return strat_; }
  const Histogram<>& latency() const noexcept { return latency_; }
  std::uint64_t processed() const noexcept { return processed_; }
  std::uint64_t orders_pushed() const noexcept { return orders_pushed_; }

 private:
  ImbalanceStrategy<NumTicks> strat_;
  FeedQ* feed_;
  OrderQ* orders_;
  FillQ* fills_;
  Histogram<> latency_;  // per market-data event: dequeue -> decision (-> push)
  std::uint64_t processed_ = 0;
  std::uint64_t orders_pushed_ = 0;
};

}  // namespace hft

#include "hft/affinity.hpp"

namespace hft {

template <std::size_t NumTicks, typename FeedQ, typename OrderQ, typename FillQ>
void StrategyRunner<NumTicks, FeedQ, OrderQ, FillQ>::run(
    const std::atomic<bool>& running, int core) {
  pin_current_thread(core);
  set_current_thread_name("hft-strat");
  while (running.load(std::memory_order_relaxed)) {
    poll_once();
  }
}

}  // namespace hft
