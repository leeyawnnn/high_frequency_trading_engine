// Phase 8: pre-trade risk gate.
#include "test_harness.hpp"
#include "hft/itch_message.hpp"
#include "hft/order_msg.hpp"
#include "hft/risk_gate.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

hft::OrderRequest order(hft::Side side, std::uint32_t size) {
  hft::OrderRequest o{};
  o.side = static_cast<std::uint8_t>(side);
  o.size = size;
  o.action = static_cast<std::uint8_t>(hft::OrderAction::kNew);
  o.symbol = hft::pack_symbol("TST");
  return o;
}

hft::ExecReport fill(hft::Side side, std::uint32_t size) {
  hft::ExecReport r{};
  r.side = static_cast<std::uint8_t>(side);
  r.fill_size = size;
  return r;
}

hft::RiskLimits limits() {
  return hft::RiskLimits{/*max_position*/ 1000, /*max_order_size*/ 500,
                         /*max_orders_per_sec*/ 0.0, /*burst*/ 0};
}

}  // namespace

HFT_TEST(order_size_limit) {
  std::atomic<bool> kill{false};
  hft::RiskGate g(limits(), kill);
  CHECK(g.check(order(hft::Side::kBuy, 0)) == hft::RiskResult::kOrderSize);    // zero
  CHECK(g.check(order(hft::Side::kBuy, 501)) == hft::RiskResult::kOrderSize);  // over cap
  CHECK(g.check(order(hft::Side::kBuy, 500)) == hft::RiskResult::kAccept);     // at cap
  CHECK_EQ(g.rejected_size(), 2u);
}

HFT_TEST(position_limit_both_directions) {
  std::atomic<bool> kill{false};
  hft::RiskGate g(limits(), kill);  // max_position 1000
  // Drive position to +900 via fills.
  g.on_fill(fill(hft::Side::kBuy, 900));
  CHECK_EQ(g.position(), 900);
  CHECK(g.allow(order(hft::Side::kBuy, 100)));                                  // ->1000 ok
  CHECK(g.check(order(hft::Side::kBuy, 200)) == hft::RiskResult::kPositionLimit);  // ->1100 no
  // Selling is fine (reduces exposure).
  CHECK(g.allow(order(hft::Side::kSell, 500)));
  // Now near the short bound.
  g.on_fill(fill(hft::Side::kSell, 1800));  // position -900
  CHECK_EQ(g.position(), -900);
  CHECK(g.check(order(hft::Side::kSell, 200)) == hft::RiskResult::kPositionLimit);  // ->-1100 no
}

HFT_TEST(kill_switch_blocks_everything) {
  std::atomic<bool> kill{false};
  hft::RiskGate g(limits(), kill);
  CHECK(g.allow(order(hft::Side::kBuy, 10)));
  kill.store(true);
  CHECK(g.check(order(hft::Side::kBuy, 10)) == hft::RiskResult::kKillSwitch);
  CHECK(g.check(order(hft::Side::kSell, 10)) == hft::RiskResult::kKillSwitch);
  kill.store(false);
  CHECK(g.allow(order(hft::Side::kBuy, 10)));  // recovers
}

HFT_TEST(rate_limit_token_bucket) {
  hft::tsc_calibrate();
  std::atomic<bool> kill{false};
  // 100 orders/sec, burst of 3.
  hft::RiskGate g(hft::RiskLimits{1'000'000, 1000, 100.0, 3}, kill);

  // Fire 50 back-to-back: only a small burst should pass.
  int accepted = 0, rate_rejected = 0;
  for (int i = 0; i < 50; ++i) {
    const hft::RiskResult r = g.check(order(hft::Side::kBuy, 1));
    if (r == hft::RiskResult::kAccept) ++accepted;
    else if (r == hft::RiskResult::kRateLimit) ++rate_rejected;
  }
  CHECK(accepted >= 1);
  CHECK(accepted <= 6);          // burst-bounded, not all 50
  CHECK(rate_rejected > 0);      // limiter actually fired

  // After ~30ms (3 tokens at 100/s) at least one more should be allowed.
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  CHECK(g.allow(order(hft::Side::kBuy, 1)));
}

HFT_TEST(accept_path_counts) {
  std::atomic<bool> kill{false};
  hft::RiskGate g(limits(), kill);
  for (int i = 0; i < 5; ++i) CHECK(g.allow(order(hft::Side::kBuy, 1)));
  CHECK_EQ(g.accepted(), 5u);
  CHECK_EQ(g.rejected_kill(), 0u);
}

HFT_TEST_MAIN()
