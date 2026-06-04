// Phase 4: UDP transport + order/exec wire formats.
// Loopback round-trip on ephemeral ports (works on macOS and Linux).
#include "test_harness.hpp"
#include "hft/itch_message.hpp"
#include "hft/net.hpp"
#include "hft/order_msg.hpp"

#include <cstring>

HFT_TEST(order_exec_formats_locked) {
  CHECK_EQ(sizeof(hft::OrderRequest), 40u);
  CHECK_EQ(sizeof(hft::ExecReport), 40u);
  CHECK_EQ(hft::kOrderSize, 40u);
}

HFT_TEST(udp_itch_roundtrip_loopback) {
  // Receiver on an OS-assigned ephemeral port (bind port 0, then read it back).
  hft::UdpSocket rx = hft::UdpSocket::bound(0, "127.0.0.1");
  ::sockaddr_in sa{};
  ::socklen_t slen = sizeof(sa);
  CHECK(::getsockname(rx.fd(), reinterpret_cast<::sockaddr*>(&sa), &slen) == 0);
  const std::uint16_t port = ntohs(sa.sin_port);
  CHECK(port != 0);

  hft::UdpSocket tx;
  const hft::Endpoint dst = hft::Endpoint::v4("127.0.0.1", port);

  hft::ItchMessage sent{};
  sent.timestamp = 0x1122334455667788ULL;
  sent.price = hft::price_from_double(99.95);
  sent.symbol = hft::pack_symbol("TST");
  sent.size = 4242;
  sent.seq = 1;
  sent.type = static_cast<std::uint8_t>(hft::MsgType::kAdd);
  sent.side = static_cast<std::uint8_t>(hft::Side::kBuy);

  const long s = tx.send_to(dst, &sent, hft::kMsgSize);
  CHECK_EQ(s, static_cast<long>(hft::kMsgSize));

  hft::ItchMessage got{};
  const long r = rx.try_recv(&got, sizeof(got));  // blocking (rx is blocking)
  CHECK_EQ(r, static_cast<long>(hft::kMsgSize));
  CHECK(std::memcmp(&sent, &got, hft::kMsgSize) == 0);
  CHECK_EQ(got.size, 4242u);
}

HFT_TEST(udp_order_fill_roundtrip_loopback) {
  // Simulate the engine<->exchange order/fill exchange both directions.
  hft::UdpSocket exch_rx = hft::UdpSocket::bound(0, "127.0.0.1");
  ::sockaddr_in sa{};
  ::socklen_t slen = sizeof(sa);
  ::getsockname(exch_rx.fd(), reinterpret_cast<::sockaddr*>(&sa), &slen);
  const std::uint16_t order_port = ntohs(sa.sin_port);

  hft::UdpSocket engine_rx = hft::UdpSocket::bound(0, "127.0.0.1");
  ::getsockname(engine_rx.fd(), reinterpret_cast<::sockaddr*>(&sa), &slen);
  const std::uint16_t fill_port = ntohs(sa.sin_port);

  hft::UdpSocket engine_tx, exch_tx;

  // Engine sends an order to the exchange.
  hft::OrderRequest ord{};
  ord.order_id = 777;
  ord.md_timestamp = 0xCAFEBABEULL;
  ord.price = hft::price_from_double(100.00);
  ord.symbol = hft::pack_symbol("TST");
  ord.size = 100;
  ord.side = static_cast<std::uint8_t>(hft::Side::kBuy);
  ord.action = static_cast<std::uint8_t>(hft::OrderAction::kNew);
  CHECK_EQ(engine_tx.send_to(hft::Endpoint::v4("127.0.0.1", order_port), &ord, sizeof(ord)),
           static_cast<long>(sizeof(ord)));

  hft::OrderRequest recv_ord{};
  CHECK_EQ(exch_rx.try_recv(&recv_ord, sizeof(recv_ord)), static_cast<long>(sizeof(recv_ord)));
  CHECK_EQ(recv_ord.order_id, 777u);

  // Exchange replies with a fill, echoing the e2e timestamp tag.
  hft::ExecReport rep{};
  rep.order_id = recv_ord.order_id;
  rep.md_timestamp = recv_ord.md_timestamp;
  rep.fill_price = recv_ord.price;
  rep.symbol = recv_ord.symbol;
  rep.fill_size = recv_ord.size;
  rep.side = recv_ord.side;
  rep.status = static_cast<std::uint8_t>(hft::ExecStatus::kFilled);
  CHECK_EQ(exch_tx.send_to(hft::Endpoint::v4("127.0.0.1", fill_port), &rep, sizeof(rep)),
           static_cast<long>(sizeof(rep)));

  hft::ExecReport recv_rep{};
  CHECK_EQ(engine_rx.try_recv(&recv_rep, sizeof(recv_rep)), static_cast<long>(sizeof(recv_rep)));
  CHECK_EQ(recv_rep.order_id, 777u);
  CHECK_EQ(recv_rep.md_timestamp, 0xCAFEBABEULL);  // tag survived the round trip
  CHECK(hft::exec_status(recv_rep) == hft::ExecStatus::kFilled);
}

HFT_TEST(try_recv_would_block_returns_negative) {
  hft::UdpSocket rx = hft::UdpSocket::bound(0, "127.0.0.1");
  rx.set_nonblocking(true);
  char buf[64];
  CHECK(rx.try_recv(buf, sizeof(buf)) < 0);  // nothing queued -> EWOULDBLOCK
}

HFT_TEST_MAIN()
