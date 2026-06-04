// The wired engine: three pinned threads connected by lock-free SPSC queues.
//
//   feed core              strategy core            gateway core
//   ┌──────────┐  feed_q   ┌───────────┐  order_q   ┌───────────┐
//   │  Feed    │═════════► │ Strategy  │═════════►  │  Gateway  │═══► wire
//   │ Handler  │           │  + book   │            │  + risk   │
//   └──────────┘           └───────────┘            └─────┬─────┘
//        ▲                       ▲   fill_q                │ fills in
//        │ feed (UDP)            └═════════════════════════┘
//
// Every market-data message is stamped with its arrival TSC in the feed
// handler; the strategy copies that stamp into the order it emits; the gateway
// computes feed-arrival → wire latency at send time. The three per-stage
// histograms plus the end-to-end one are exposed for reporting.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "hft/feed_handler.hpp"
#include "hft/order_gateway.hpp"
#include "hft/order_msg.hpp"
#include "hft/risk_gate.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/strategy.hpp"

namespace hft {

struct EngineConfig {
  // Core pinning (-1 = unpinned).
  int feed_core = -1;
  int strategy_core = -1;
  int gateway_core = -1;

  // Networking.
  std::uint16_t feed_port = 31337;          // feed handler binds (0 = ephemeral)
  Endpoint order_dst = Endpoint::v4("127.0.0.1", 31338);  // gateway -> exchange
  std::uint16_t fill_port = 31339;          // gateway binds (0 = ephemeral)
  const char* feed_bind = "0.0.0.0";
  const char* fill_bind = "0.0.0.0";

  // Strategy / book. base_price is the bottom (index 0) of the flat-array book
  // window, so it must sit BELOW the lowest price the feed will quote. The
  // exchange's mid starts at 100.00, so 80.00 leaves ~20 dollars of headroom
  // below and (with 4096 cent-ticks) ~21 above.
  std::int64_t base_price = price_from_double(80.0);
  std::int64_t tick = kPriceScale / 100;
  double threshold = 0.30;
  std::uint32_t order_size = 100;

  // Risk.
  RiskLimits risk{/*max_position*/ 1000, /*max_order_size*/ 500,
                  /*rate*/ 0.0, /*burst*/ 0};
};

template <std::size_t NumTicks = 4096,
          std::size_t FeedQCap = (1u << 16),
          std::size_t OrderQCap = (1u << 12),
          std::size_t FillQCap = (1u << 12)>
class Engine {
 public:
  using FeedQ = SpscQueue<MdEvent, FeedQCap>;
  using OrderQ = SpscQueue<OrderRequest, OrderQCap>;
  using FillQ = SpscQueue<ExecReport, FillQCap>;
  using Feed = FeedHandler<FeedQ>;
  using Strat = StrategyRunner<NumTicks, FeedQ, OrderQ, FillQ>;
  using Gateway = OrderGateway<OrderQ, FillQ>;

  explicit Engine(const EngineConfig& cfg)
      : cfg_(cfg),
        feed_(cfg.feed_port, feed_q_, cfg.feed_bind),
        strat_(StrategyParams{cfg.base_price, cfg.tick, cfg.threshold, cfg.order_size},
               feed_q_, order_q_, &fill_q_),
        gw_(order_q_, fill_q_, cfg.order_dst, cfg.fill_port, cfg.risk, kill_,
            cfg.fill_bind) {}

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Port wiring for in-process (ephemeral) setups.
  std::uint16_t feed_local_port() const { return feed_.local_port(); }
  std::uint16_t fill_local_port() const { return gw_.fill_local_port(); }
  void set_order_destination(const Endpoint& e) { gw_.set_order_destination(e); }

  void start() {
    running_.store(true, std::memory_order_relaxed);
    feed_thread_ = std::thread([this] { feed_.run(running_, cfg_.feed_core); });
    strat_thread_ = std::thread([this] { strat_.run(running_, cfg_.strategy_core); });
    gw_thread_ = std::thread([this] { gw_.run(running_, cfg_.gateway_core); });
  }

  void stop() {
    running_.store(false, std::memory_order_relaxed);
    if (gw_thread_.joinable()) gw_thread_.join();
    if (strat_thread_.joinable()) strat_thread_.join();
    if (feed_thread_.joinable()) feed_thread_.join();
  }

  void trip_kill_switch() { kill_.store(true, std::memory_order_relaxed); }

  // ---- accessors for reporting -------------------------------------------
  const Feed& feed() const { return feed_; }
  const Strat& strategy() const { return strat_; }
  const Gateway& gateway() const { return gw_; }

  const Histogram<>& feed_latency() const { return feed_.latency(); }
  const Histogram<>& strategy_latency() const { return strat_.latency(); }
  const Histogram<>& gateway_latency() const { return gw_.gateway_latency(); }
  const Histogram<>& e2e_latency() const { return gw_.e2e_latency(); }
  const Histogram<>& round_trip_latency() const { return gw_.round_trip_latency(); }

 private:
  EngineConfig cfg_;
  FeedQ feed_q_;
  OrderQ order_q_;
  FillQ fill_q_;
  std::atomic<bool> kill_{false};
  std::atomic<bool> running_{false};

  Feed feed_;
  Strat strat_;
  Gateway gw_;

  std::thread feed_thread_;
  std::thread strat_thread_;
  std::thread gw_thread_;
};

}  // namespace hft
