// Phase 7: book-imbalance strategy.
#include "test_harness.hpp"
#include "hft/feed_handler.hpp"
#include "hft/itch_message.hpp"
#include "hft/order_msg.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/strategy.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace {

constexpr std::size_t N = 4096;

hft::StrategyParams params() {
  return hft::StrategyParams{
      hft::price_from_double(90.00),  // base
      hft::kPriceScale / 100,         // 1-cent tick
      0.30,                           // threshold
      100,                            // order size
  };
}

hft::MdEvent ev(hft::MsgType t, hft::Side s, double px, std::uint32_t size,
                std::uint64_t arrival) {
  hft::MdEvent e{};
  e.msg.type = static_cast<std::uint8_t>(t);
  e.msg.side = static_cast<std::uint8_t>(s);
  e.msg.price = hft::price_from_double(px);
  e.msg.size = size;
  e.msg.symbol = hft::pack_symbol("TST");
  e.arrival_tsc = arrival;
  return e;
}

// Feed an event, capturing the emitted order (if any).
template <class Strat>
std::optional<hft::OrderRequest> feed(Strat& s, const hft::MdEvent& e) {
  std::optional<hft::OrderRequest> out;
  s.on_md(e, [&](const hft::OrderRequest& o) { out = o; });
  return out;
}

}  // namespace

HFT_TEST(no_signal_until_two_sided) {
  hft::ImbalanceStrategy<N> s(params());
  // Only a bid so far -> not two-sided -> no order.
  CHECK(!feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 900, 1)).has_value());
  CHECK_EQ(s.orders_sent(), 0u);
}

HFT_TEST(bid_heavy_triggers_buy_at_bid) {
  hft::ImbalanceStrategy<N> s(params());
  feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 900, 11));
  // Adding the ask makes the book two-sided; imbalance = (900-100)/1000 = 0.8.
  auto o = feed(s, ev(hft::MsgType::kAdd, hft::Side::kSell, 100.01, 100, 22));
  CHECK(o.has_value());
  CHECK_EQ(o->side, static_cast<std::uint8_t>(hft::Side::kBuy));
  CHECK_EQ(o->price, hft::price_from_double(100.00));   // buy AT THE BID
  CHECK_EQ(o->size, 100u);
  CHECK_EQ(o->md_timestamp, 22u);                       // carried e2e tag
  CHECK_EQ(o->action, static_cast<std::uint8_t>(hft::OrderAction::kNew));
  CHECK(s.in_flight());
}

HFT_TEST(no_double_order_while_in_flight) {
  hft::ImbalanceStrategy<N> s(params());
  feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 900, 1));
  CHECK(feed(s, ev(hft::MsgType::kAdd, hft::Side::kSell, 100.01, 100, 2)).has_value());
  // Still bid-heavy, but an order is in flight -> no second order.
  CHECK(!feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 50, 3)).has_value());
  CHECK_EQ(s.orders_sent(), 1u);
}

HFT_TEST(fill_updates_position_then_opposite_signal_flattens) {
  hft::ImbalanceStrategy<N> s(params());
  feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 900, 1));
  auto buy = feed(s, ev(hft::MsgType::kAdd, hft::Side::kSell, 100.01, 100, 2));
  CHECK(buy.has_value());

  // Fill the buy -> long 100, no longer in flight.
  hft::ExecReport f{};
  f.side = buy->side;
  f.fill_size = buy->size;
  s.on_fill(f);
  CHECK_EQ(s.position(), 100);
  CHECK(!s.in_flight());

  // Make the book ask-heavy by shrinking the bid: imbalance (50-100)/150 = -0.33
  // < -0.30. The Execute is re-evaluated immediately, so the SELL fires on it.
  auto sell = feed(s, ev(hft::MsgType::kExecute, hft::Side::kBuy, 100.00, 850, 3));  // bid -> 50
  CHECK(sell.has_value());
  CHECK_EQ(sell->side, static_cast<std::uint8_t>(hft::Side::kSell));
  CHECK_EQ(sell->price, hft::price_from_double(100.01));  // sell AT THE ASK

  hft::ExecReport f2{};
  f2.side = sell->side;
  f2.fill_size = sell->size;
  s.on_fill(f2);
  CHECK_EQ(s.position(), 0);  // flat again
}

HFT_TEST(threshold_boundary) {
  hft::ImbalanceStrategy<N> s(params());  // effective T = 4915/16384 = 0.29999
  feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 129, 1));
  // (129-71)/200 = 0.29 < T -> no order.
  CHECK(!feed(s, ev(hft::MsgType::kAdd, hft::Side::kSell, 100.01, 71, 2)).has_value());
  // Add 5 to the bid: (134-71)/205 = 0.307 > T -> order.
  CHECK(feed(s, ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 5, 3)).has_value());
}

HFT_TEST(runner_through_real_queues) {
  using FeedQ = hft::SpscQueue<hft::MdEvent, 1024>;
  using OrderQ = hft::SpscQueue<hft::OrderRequest, 1024>;
  using FillQ = hft::SpscQueue<hft::ExecReport, 64>;
  auto feedq = std::make_unique<FeedQ>();
  auto orderq = std::make_unique<OrderQ>();
  auto fillq = std::make_unique<FillQ>();

  hft::StrategyRunner<N, FeedQ, OrderQ, FillQ> runner(params(), *feedq, *orderq, fillq.get());

  // Push a bid-heavy two-sided book onto the feed queue.
  CHECK(feedq->push(ev(hft::MsgType::kAdd, hft::Side::kBuy, 100.00, 900, 100)));
  CHECK(feedq->push(ev(hft::MsgType::kAdd, hft::Side::kSell, 100.01, 100, 200)));

  while (runner.poll_once()) { /* drain */ }

  hft::OrderRequest o{};
  CHECK(orderq->pop(o));                                  // an order was produced
  CHECK_EQ(o.side, static_cast<std::uint8_t>(hft::Side::kBuy));
  CHECK_EQ(o.md_timestamp, 200u);
  CHECK_EQ(runner.latency().count(), 2u);                // both MD events timed
  CHECK_EQ(runner.orders_pushed(), 1u);

  // Feed a fill back; runner applies it on the next poll.
  hft::ExecReport f{};
  f.side = o.side;
  f.fill_size = o.size;
  CHECK(fillq->push(f));
  runner.poll_once();                                    // no MD, but drains fill
  CHECK_EQ(runner.strategy().position(), 100);
}

HFT_TEST_MAIN()
