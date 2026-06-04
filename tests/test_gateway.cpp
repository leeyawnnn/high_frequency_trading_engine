// Phase 9: order gateway (order queue -> risk -> wire, + fill intake).
#include "test_harness.hpp"
#include "hft/itch_message.hpp"
#include "hft/net.hpp"
#include "hft/order_gateway.hpp"
#include "hft/order_msg.hpp"
#include "hft/risk_gate.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace {
using OrderQ = hft::SpscQueue<hft::OrderRequest, 1024>;
using FillQ = hft::SpscQueue<hft::ExecReport, 1024>;

hft::OrderRequest order(std::uint64_t id, hft::Side side, std::uint32_t size,
                        double px, std::uint64_t md_ts) {
  hft::OrderRequest o{};
  o.order_id = id;
  o.md_timestamp = md_ts;
  o.side = static_cast<std::uint8_t>(side);
  o.size = size;
  o.price = hft::price_from_double(px);
  o.symbol = hft::pack_symbol("TST");
  o.action = static_cast<std::uint8_t>(hft::OrderAction::kNew);
  return o;
}

hft::RiskLimits limits() {
  return hft::RiskLimits{/*max_position*/ 10000, /*max_order_size*/ 500,
                         /*rate*/ 0.0, /*burst*/ 0};
}
}  // namespace

HFT_TEST(sends_orders_and_tracks_inflight) {
  hft::tsc_calibrate();
  auto orderq = std::make_unique<OrderQ>();
  auto fillq = std::make_unique<FillQ>();
  std::atomic<bool> kill{false};

  // A "test exchange": socket that receives the orders the gateway sends.
  hft::UdpSocket exch = hft::UdpSocket::bound(0, "127.0.0.1");
  const hft::Endpoint exch_ep = hft::Endpoint::v4("127.0.0.1", exch.local_port());

  hft::OrderGateway<OrderQ, FillQ> gw(*orderq, *fillq, exch_ep, /*fill_port*/ 0,
                                      limits(), kill, "127.0.0.1");

  // Push three orders; drive the gateway.
  orderq->push(order(1, hft::Side::kBuy, 100, 100.00, hft::tsc_now()));
  orderq->push(order(2, hft::Side::kSell, 200, 100.01, hft::tsc_now()));
  orderq->push(order(3, hft::Side::kBuy, 50, 99.99, hft::tsc_now()));
  while (gw.poll_once()) { /* drain */ }

  CHECK_EQ(gw.sent(), 3u);
  CHECK_EQ(gw.risk_rejected(), 0u);
  CHECK_EQ(gw.gateway_latency().count(), 3u);
  CHECK_EQ(gw.e2e_latency().count(), 3u);

  // The exchange must have received exactly those three orders, in order.
  for (std::uint64_t expect_id = 1; expect_id <= 3; ++expect_id) {
    hft::OrderRequest got{};
    CHECK_EQ(exch.try_recv(&got, sizeof(got)), static_cast<long>(hft::kOrderSize));
    CHECK_EQ(got.order_id, expect_id);
  }
}

HFT_TEST(fill_matches_inflight_updates_risk_and_forwards) {
  hft::tsc_calibrate();
  auto orderq = std::make_unique<OrderQ>();
  auto fillq = std::make_unique<FillQ>();
  std::atomic<bool> kill{false};

  hft::UdpSocket exch = hft::UdpSocket::bound(0, "127.0.0.1");
  const hft::Endpoint exch_ep = hft::Endpoint::v4("127.0.0.1", exch.local_port());

  hft::OrderGateway<OrderQ, FillQ> gw(*orderq, *fillq, exch_ep, 0, limits(), kill,
                                      "127.0.0.1");
  const std::uint16_t gw_fill_port = gw.fill_local_port();
  CHECK(gw_fill_port != 0);

  const std::uint64_t md_ts = hft::tsc_now();
  orderq->push(order(42, hft::Side::kBuy, 300, 100.00, md_ts));
  gw.poll_once();
  CHECK_EQ(gw.sent(), 1u);

  // The exchange consumes the order and sends a fill back to the gateway.
  hft::OrderRequest recv_ord{};
  CHECK_EQ(exch.try_recv(&recv_ord, sizeof(recv_ord)), static_cast<long>(hft::kOrderSize));
  hft::ExecReport rep{};
  rep.order_id = recv_ord.order_id;
  rep.md_timestamp = recv_ord.md_timestamp;
  rep.side = recv_ord.side;
  rep.fill_size = recv_ord.size;
  rep.status = static_cast<std::uint8_t>(hft::ExecStatus::kFilled);
  hft::UdpSocket exch_tx;
  exch_tx.send_to(hft::Endpoint::v4("127.0.0.1", gw_fill_port), &rep, sizeof(rep));

  // Drive the gateway so it ingests the fill.
  for (int i = 0; i < 1000 && gw.fills_matched() == 0; ++i) gw.poll_once();

  CHECK_EQ(gw.fills_matched(), 1u);
  CHECK_EQ(gw.fills_unmatched(), 0u);
  CHECK_EQ(gw.risk().position(), 300);          // gate position updated by fill
  CHECK_EQ(gw.round_trip_latency().count(), 1u);

  // Fill forwarded to the strategy.
  hft::ExecReport forwarded{};
  CHECK(fillq->pop(forwarded));
  CHECK_EQ(forwarded.order_id, 42u);
}

HFT_TEST(risk_rejected_order_never_hits_wire) {
  hft::tsc_calibrate();
  auto orderq = std::make_unique<OrderQ>();
  auto fillq = std::make_unique<FillQ>();
  std::atomic<bool> kill{false};

  hft::UdpSocket exch = hft::UdpSocket::bound(0, "127.0.0.1");
  exch.set_nonblocking(true);
  const hft::Endpoint exch_ep = hft::Endpoint::v4("127.0.0.1", exch.local_port());

  hft::OrderGateway<OrderQ, FillQ> gw(*orderq, *fillq, exch_ep, 0, limits(), kill,
                                      "127.0.0.1");

  orderq->push(order(1, hft::Side::kBuy, 9999, 100.00, hft::tsc_now()));  // over size cap
  gw.poll_once();
  CHECK_EQ(gw.sent(), 0u);
  CHECK_EQ(gw.risk_rejected(), 1u);

  hft::OrderRequest got{};
  CHECK(exch.try_recv(&got, sizeof(got)) < 0);  // nothing on the wire

  // Kill switch blocks a valid order too.
  kill.store(true);
  orderq->push(order(2, hft::Side::kBuy, 100, 100.00, hft::tsc_now()));
  gw.poll_once();
  CHECK_EQ(gw.sent(), 0u);
  CHECK_EQ(gw.risk_rejected(), 2u);
}

HFT_TEST_MAIN()
