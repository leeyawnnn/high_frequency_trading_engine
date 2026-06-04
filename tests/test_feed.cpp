// Phase 5: feed-handler stage (socket -> parse -> tag -> SPSC).
#include "test_harness.hpp"
#include "hft/affinity.hpp"
#include "hft/feed_handler.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"
#include "hft/net.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/tsc.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
using Queue = hft::SpscQueue<hft::MdEvent, 4096>;

hft::ItchMessage make(std::uint32_t seq) {
  hft::ItchMessage m{};
  m.timestamp = 0x1000 + seq;
  m.price = hft::price_from_double(100.0) + seq;
  m.symbol = hft::pack_symbol("TST");
  m.size = 100 + seq;
  m.seq = seq;
  m.type = static_cast<std::uint8_t>(hft::MsgType::kAdd);
  m.side = static_cast<std::uint8_t>(hft::Side::kBuy);
  return m;
}
}  // namespace

HFT_TEST(affinity_helpers_are_safe) {
  // Pinning to an invalid core returns false; naming never crashes. (On macOS
  // pinning is always a no-op returning false.)
  CHECK(!hft::pin_current_thread(-1));
  hft::set_current_thread_name("hft-test");
}

HFT_TEST(feed_handler_single_and_batched) {
  hft::tsc_calibrate();
  auto q = std::make_unique<Queue>();
  hft::FeedHandler<Queue> fh(0, *q, "127.0.0.1");  // ephemeral port
  const std::uint16_t port = fh.local_port();
  CHECK(port != 0);

  hft::UdpSocket tx;
  const hft::Endpoint dst = hft::Endpoint::v4("127.0.0.1", port);

  constexpr std::uint32_t kN = 100;
  std::vector<hft::ItchMessage> sent;
  sent.reserve(kN);

  // First 40 as one-message datagrams.
  std::uint32_t seq = 0;
  for (; seq < 40; ++seq) {
    hft::ItchMessage m = make(seq);
    sent.push_back(m);
    std::byte d[hft::kMsgSize];
    hft::encode_message(m, d);
    tx.send_to(dst, d, sizeof(d));
  }
  // Remaining 60 as batched datagrams of 10 messages each.
  while (seq < kN) {
    std::byte d[10 * hft::kMsgSize];
    for (std::size_t k = 0; k < 10; ++k, ++seq) {
      hft::ItchMessage m = make(seq);
      sent.push_back(m);
      hft::encode_message(m, d + k * hft::kMsgSize);
    }
    tx.send_to(dst, d, sizeof(d));
  }

  // Drain the socket via poll_once until all messages are enqueued (bounded).
  for (int spins = 0; fh.received() < kN && spins < 1'000'000; ++spins) {
    fh.poll_once();
  }

  CHECK_EQ(fh.received(), kN);
  CHECK_EQ(fh.pushed(), kN);
  CHECK_EQ(fh.dropped(), 0u);
  CHECK_EQ(fh.latency().count(), kN);  // one latency sample per message

  // Verify the queue holds exactly the sent stream, in order, with live tags.
  hft::MdEvent ev{};
  std::uint32_t expect = 0;
  while (q->pop(ev)) {
    CHECK_EQ(ev.msg.seq, expect);
    CHECK_EQ(ev.msg.size, sent[expect].size);
    CHECK_EQ(ev.msg.price, sent[expect].price);
    CHECK(ev.arrival_tsc != 0);  // tagged on arrival
    ++expect;
  }
  CHECK_EQ(expect, kN);
}

HFT_TEST_MAIN()
