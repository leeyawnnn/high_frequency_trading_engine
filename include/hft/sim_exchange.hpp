// Reusable simulated exchange: generates an ITCH-style top-of-book feed and
// answers orders with fills. Factored out of the exchange_sim tool so the
// self-contained latency harness can run the same exchange in-process.
//
// Transport is loopback UDP. Feed is a random walk of the top of book with
// deliberate size imbalances and occasional crosses, each refresh emitting a
// Delete of the old level then an Add of the new (so a level book stays
// coherent). Messages are packed `batch` per datagram to amortize sendto.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"
#include "hft/net.hpp"
#include "hft/order_msg.hpp"
#include "hft/tsc.hpp"

namespace hft {

namespace detail {
// xorshift128+ — fast, deterministic, non-crypto.
struct Rng {
  std::uint64_t s0, s1;
  explicit Rng(std::uint64_t seed)
      : s0(seed ^ 0x9E3779B97F4A7C15ULL), s1(seed * 0xD1B54A32D192ED03ULL + 1) {}
  std::uint64_t next() {
    std::uint64_t x = s0, y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
  }
  std::uint32_t range(std::uint32_t lo, std::uint32_t hi) {
    return lo + static_cast<std::uint32_t>(next() % (hi - lo + 1));
  }
};
}  // namespace detail

class SimExchange {
 public:
  struct Config {
    Endpoint feed_dst;             // where to publish the feed
    Endpoint fill_dst;             // where to publish fills
    std::uint16_t order_bind_port; // where to listen for orders (0 = ephemeral)
    std::uint32_t symbol;
    long rate;                     // feed msgs/sec (0 = max)
    long batch;                    // messages per datagram (1..40)
    std::uint64_t seed;
    std::int64_t base_price;       // starting mid (fixed point)
    std::int64_t tick;             // tick size (fixed point)
  };

  explicit SimExchange(const Config& c)
      : feed_dst_(c.feed_dst),
        fill_dst_(c.fill_dst),
        order_rx_(UdpSocket::bound(c.order_bind_port)),
        sym_(c.symbol),
        batch_(c.batch < 1 ? 1 : (c.batch > 40 ? 40 : c.batch)),
        tick_(c.tick),
        rng_(c.seed),
        mid_(c.base_price),
        best_bid_(c.base_price - c.tick),
        best_ask_(c.base_price + c.tick),
        mid_lo_(c.base_price - 1500 * c.tick),  // keep the walk in a +/-$15 band
        mid_hi_(c.base_price + 1500 * c.tick) {  // so quotes stay in the book window
    order_rx_.set_nonblocking(true);
    feed_tx_.set_send_buffer(1 << 20);
    const double cyc_per_ns = tsc_calibration().cycles_per_ns;
    cycles_per_msg_ = (c.rate > 0)
        ? static_cast<std::uint64_t>(1e9 / static_cast<double>(c.rate) * cyc_per_ns)
        : 0;
  }

  std::uint16_t order_local_port() const { return order_rx_.local_port(); }
  void set_feed_dst(const Endpoint& e) { feed_dst_ = e; }
  void set_fill_dst(const Endpoint& e) { fill_dst_ = e; }

  std::uint64_t feed_count() const noexcept { return feed_count_; }
  std::uint64_t order_count() const noexcept { return order_count_; }
  std::uint64_t fill_count() const noexcept { return fill_count_; }

  // Run until `running` clears.
  void run(const std::atomic<bool>& running) {
    next_send_ = tsc_now();
    emit(MsgType::kAdd, Side::kBuy, best_bid_, bid_sz_);
    emit(MsgType::kAdd, Side::kSell, best_ask_, ask_sz_);
    flush();
    while (running.load(std::memory_order_relaxed)) {
      step();
      service_orders();
    }
    flush();
  }

 private:
  void step() {
    const bool do_bid = (rng_.next() & 1) != 0;
    const int s = static_cast<int>(rng_.range(0, 2)) - 1;  // -1,0,+1 tick
    mid_ += static_cast<std::int64_t>(s) * tick_;
    if (mid_ < mid_lo_) mid_ = mid_lo_;  // reflect at the band edges so the
    if (mid_ > mid_hi_) mid_ = mid_hi_;  // top of book never leaves the window
    const bool cross = rng_.range(0, 255) == 0;
    const std::int64_t spread = cross ? -tick_ : tick_;

    if (do_bid) {
      emit(MsgType::kDelete, Side::kBuy, best_bid_, bid_sz_);
      best_bid_ = mid_ - spread;
      bid_sz_ = (rng_.range(0, 15) == 0) ? rng_.range(2000, 5000) : rng_.range(100, 800);
      emit(MsgType::kAdd, Side::kBuy, best_bid_, bid_sz_);
    } else {
      emit(MsgType::kDelete, Side::kSell, best_ask_, ask_sz_);
      best_ask_ = mid_ + spread;
      ask_sz_ = (rng_.range(0, 15) == 0) ? rng_.range(2000, 5000) : rng_.range(100, 800);
      emit(MsgType::kAdd, Side::kSell, best_ask_, ask_sz_);
    }
  }

  void emit(MsgType type, Side side, std::int64_t px, std::uint32_t size) {
    if (cycles_per_msg_) {
      while (tsc_now() < next_send_) { /* pace */ }
      next_send_ += cycles_per_msg_;
    }
    ItchMessage m{};
    m.timestamp = tsc_now();
    m.price = px;
    m.symbol = sym_;
    m.size = size;
    m.seq = seq_++;
    m.type = static_cast<std::uint8_t>(type);
    m.side = static_cast<std::uint8_t>(side);
    encode_message(m, dgram_.data() + static_cast<std::size_t>(dgram_msgs_) * kMsgSize);
    ++dgram_msgs_;
    ++feed_count_;
    if (dgram_msgs_ >= batch_) flush();
  }

  void flush() {
    if (dgram_msgs_ > 0) {
      feed_tx_.send_to(feed_dst_, dgram_.data(),
                       static_cast<std::size_t>(dgram_msgs_) * kMsgSize);
      dgram_msgs_ = 0;
    }
  }

  void service_orders() {
    OrderRequest ord{};
    for (int d = 0; d < 32; ++d) {
      const long n = order_rx_.try_recv(&ord, sizeof(ord));
      if (n != static_cast<long>(kOrderSize)) break;
      ++order_count_;
      ExecReport rep{};
      rep.order_id = ord.order_id;
      rep.md_timestamp = ord.md_timestamp;  // echo the e2e tag
      rep.fill_price = ord.price;
      rep.symbol = ord.symbol;
      rep.fill_size = ord.size;
      rep.side = ord.side;
      rep.status = static_cast<std::uint8_t>(ExecStatus::kFilled);
      fill_tx_.send_to(fill_dst_, &rep, sizeof(rep));
      ++fill_count_;
    }
  }

  UdpSocket feed_tx_;
  UdpSocket fill_tx_;
  Endpoint feed_dst_;
  Endpoint fill_dst_;
  UdpSocket order_rx_;
  std::uint32_t sym_;
  long batch_;
  std::int64_t tick_;
  detail::Rng rng_;
  std::int64_t mid_;
  std::int64_t best_bid_;
  std::int64_t best_ask_;
  std::int64_t mid_lo_;
  std::int64_t mid_hi_;
  std::uint32_t bid_sz_ = 500;
  std::uint32_t ask_sz_ = 500;
  std::uint32_t seq_ = 0;
  std::uint64_t cycles_per_msg_ = 0;
  std::uint64_t next_send_ = 0;
  std::array<std::byte, 40 * kMsgSize> dgram_{};
  long dgram_msgs_ = 0;
  std::uint64_t feed_count_ = 0;
  std::uint64_t order_count_ = 0;
  std::uint64_t fill_count_ = 0;
};

}  // namespace hft
