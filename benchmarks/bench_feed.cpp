// Phase 5 benchmark: feed-handler throughput + recv->enqueue latency.
//
// Pipeline under test (all real, over loopback UDP):
//   sender thread  --batched datagrams-->  FeedHandler thread  --SPSC-->  drain
//
// The sender blasts (optionally rate-limited) batched datagrams; the feed
// handler receives/parses/tags/enqueues on its own core; a drain thread empties
// the queue and checks for sequence gaps (= dropped messages). We report
// sustained msg/sec, the drop count, and the feed handler's p50/p99/p999.
//
// Usage: bench_feed [total_msgs] [batch] [rate_msgs_per_sec(0=max)]
//                   [feed_core] [drain_core]
#include "hft/feed_handler.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"
#include "hft/net.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/affinity.hpp"
#include "hft/tsc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {
using Queue = hft::SpscQueue<hft::MdEvent, (1u << 16)>;
}

int main(int argc, char** argv) {
  hft::tsc_calibrate();

  const std::uint64_t total = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000ULL;
  long batch = (argc > 2) ? std::strtol(argv[2], nullptr, 10) : 32;
  batch = std::clamp(batch, 1L, 40L);
  const long rate = (argc > 3) ? std::strtol(argv[3], nullptr, 10) : 0;  // 0 = max
  const int feed_core = (argc > 4) ? static_cast<int>(std::strtol(argv[4], nullptr, 10)) : -1;
  const int drain_core = (argc > 5) ? static_cast<int>(std::strtol(argv[5], nullptr, 10)) : -1;

  auto q = std::make_unique<Queue>();
  hft::FeedHandler<Queue> fh(0, *q, "127.0.0.1");
  const std::uint16_t port = fh.local_port();

  std::atomic<bool> fh_running{true};
  std::atomic<bool> sender_done{false};
  std::atomic<std::uint64_t> received{0};

  std::thread feed_thread([&] { fh.run(fh_running, feed_core); });

  std::thread drain_thread([&] {
    hft::pin_current_thread(drain_core);
    hft::MdEvent ev{};
    std::uint64_t got = 0;
    std::uint64_t idle_start = hft::tsc_now();
    const double idle_ns_limit = 300e6;  // give up 300ms after sender finishes
    while (got < total) {
      if (q->pop(ev)) {
        ++got;
        idle_start = hft::tsc_now();
      } else if (sender_done.load(std::memory_order_relaxed) &&
                 hft::tsc_to_ns(hft::tsc_now() - idle_start) > idle_ns_limit) {
        break;  // remaining messages were dropped in the kernel
      }
    }
    received.store(got, std::memory_order_relaxed);
  });

  // Let the feed handler reach its poll loop before we start the clock.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  hft::UdpSocket tx;
  tx.set_send_buffer(1 << 22);
  const hft::Endpoint dst = hft::Endpoint::v4("127.0.0.1", port);

  const double cyc_per_ns = hft::tsc_calibration().cycles_per_ns;
  const std::uint64_t cycles_per_batch =
      (rate > 0)
          ? static_cast<std::uint64_t>(static_cast<double>(batch) * 1e9 /
                                       static_cast<double>(rate) * cyc_per_ns)
          : 0;

  std::array<std::byte, 40 * hft::kMsgSize> dgram{};
  std::uint64_t next_send = hft::tsc_now();
  const std::uint64_t t0 = hft::tsc_now_serialized();

  std::uint64_t seq = 0;
  while (seq < total) {
    const std::uint64_t n = std::min<std::uint64_t>(static_cast<std::uint64_t>(batch), total - seq);
    for (std::uint64_t k = 0; k < n; ++k) {
      hft::ItchMessage m{};
      m.seq = static_cast<std::uint32_t>(seq + k);
      m.symbol = hft::pack_symbol("TST");
      m.size = 100;
      m.type = static_cast<std::uint8_t>(hft::MsgType::kAdd);
      hft::encode_message(m, dgram.data() + k * hft::kMsgSize);
    }
    if (cycles_per_batch) {
      while (hft::tsc_now() < next_send) { /* pace */ }
      next_send += cycles_per_batch;
    }
    tx.send_to(dst, dgram.data(), n * hft::kMsgSize);
    seq += n;
  }
  sender_done.store(true, std::memory_order_relaxed);

  drain_thread.join();
  const std::uint64_t t1 = hft::tsc_now_serialized();
  fh_running.store(false, std::memory_order_relaxed);
  feed_thread.join();

  const double ns = hft::tsc_to_ns(t1 - t0);
  const std::uint64_t got = received.load();
  const std::uint64_t drops = total - got;
  const auto& h = fh.latency();

  std::printf("feed-handler benchmark\n");
  std::printf("  sent        : %llu msgs (batch=%ld, rate=%s)\n",
              static_cast<unsigned long long>(total), batch,
              rate > 0 ? "limited" : "max");
  std::printf("  received    : %llu  (drops: %llu, %.4f%%)\n",
              static_cast<unsigned long long>(got),
              static_cast<unsigned long long>(drops),
              100.0 * static_cast<double>(drops) / static_cast<double>(total));
  std::printf("  elapsed     : %.1f ms\n", ns / 1e6);
  std::printf("  throughput  : %.2f M msg/sec\n",
              static_cast<double>(total) / (ns / 1e9) / 1e6);
  std::printf("  feed recv->enqueue latency (ns):\n");
  std::printf("    p50=%llu  p99=%llu  p999=%llu  max=%llu  (n=%llu)\n",
              static_cast<unsigned long long>(h.percentile(50.0)),
              static_cast<unsigned long long>(h.percentile(99.0)),
              static_cast<unsigned long long>(h.percentile(99.9)),
              static_cast<unsigned long long>(h.max()),
              static_cast<unsigned long long>(h.count()));
  std::printf("  queue-full drops in handler: %llu\n",
              static_cast<unsigned long long>(fh.dropped()));
  return 0;
}
