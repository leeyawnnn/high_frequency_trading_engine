// Feed-handler stage: socket -> parse -> tag -> SPSC queue (to the strategy).
//
// Runs pinned to a dedicated core, busy-polling the UDP socket (no blocking
// recv: in HFT you spin, you don't sleep). For each datagram it stamps the
// arrival TSC once, then parses every message in the datagram (datagrams may
// carry a batch, which is how we amortize the recv syscall) and pushes a tagged
// event onto the outbound queue. The arrival stamp rides along the whole
// pipeline and anchors the end-to-end latency measurement (Phase 10).
//
// It records, per message, the time from datagram arrival to enqueue completion
// — the latency this stage adds before the strategy can see the data.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hft/compiler.hpp"
#include "hft/histogram.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"
#include "hft/net.hpp"
#include "hft/tsc.hpp"

namespace hft {

// One market-data message plus the feed handler's arrival timestamp. This is
// the payload carried on the feed -> strategy SPSC queue.
struct MdEvent {
  ItchMessage msg;            // 32 bytes
  std::uint64_t arrival_tsc;  // TSC when the datagram was received
};

template <typename OutQueue>
class FeedHandler {
 public:
  // Bind to `port` (0 = OS-chosen ephemeral, useful for tests). The outbound
  // queue must outlive the handler.
  FeedHandler(std::uint16_t port, OutQueue& out, const char* bind_addr = "0.0.0.0")
      : rx_(UdpSocket::bound(port, bind_addr)), out_(&out) {
    rx_.set_nonblocking(true);
    rx_.set_recv_buffer(1 << 22);  // 4 MiB: absorb bursts, fewer kernel drops
  }

  std::uint16_t local_port() const { return rx_.local_port(); }

  // Process at most one datagram. Returns the number of messages enqueued.
  // Exposed (vs. being buried in run()) so tests can drive it without a thread.
  HFT_ALWAYS_INLINE int poll_once() noexcept {
    const long n = rx_.try_recv(buf_.data(), buf_.size());
    if (n <= 0) return 0;  // EWOULDBLOCK or empty
    const std::uint64_t arrival = tsc_now();

    FeedReader reader(buf_.data(), static_cast<std::size_t>(n));
    int enqueued = 0;
    while (reader.has_next()) {
      ++received_;
      const MdEvent ev{reader.next(), arrival};
      if (HFT_LIKELY(push_with_backpressure(ev))) {
        ++pushed_;
        ++enqueued;
        latency_.record(tsc_to_ns_u(tsc_now() - arrival));
      } else {
        ++dropped_;  // consumer stalled past our spin budget
      }
    }
    return enqueued;
  }

  // Pin to `core`, then busy-poll until `running` clears.
  void run(const std::atomic<bool>& running, int core);

  // ---- stats / reporting (cold path) -------------------------------------
  std::uint64_t received() const noexcept { return received_; }
  std::uint64_t pushed() const noexcept { return pushed_; }
  std::uint64_t dropped() const noexcept { return dropped_; }
  const Histogram<>& latency() const noexcept { return latency_; }

 private:
  // Bounded spin so a wedged consumer can't hang the feed thread forever; under
  // normal load the very first push succeeds.
  HFT_ALWAYS_INLINE bool push_with_backpressure(const MdEvent& ev) noexcept {
    for (int spin = 0; spin < (1 << 16); ++spin)
      if (HFT_LIKELY(out_->push(ev))) return true;
    return false;
  }

  static constexpr std::size_t kMaxMsgsPerDatagram = 64;
  static constexpr std::size_t kBufBytes = kMaxMsgsPerDatagram * kMsgSize;

  UdpSocket rx_;
  OutQueue* out_;
  alignas(kCacheLineSize) std::array<std::byte, kBufBytes> buf_{};
  Histogram<> latency_;  // recv -> enqueue, nanoseconds
  std::uint64_t received_ = 0;
  std::uint64_t pushed_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace hft

#include "hft/affinity.hpp"

namespace hft {

template <typename OutQueue>
void FeedHandler<OutQueue>::run(const std::atomic<bool>& running, int core) {
  pin_current_thread(core);
  set_current_thread_name("hft-feed");
  while (running.load(std::memory_order_relaxed)) {
    poll_once();
  }
}

}  // namespace hft
