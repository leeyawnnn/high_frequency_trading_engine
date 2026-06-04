// Phase 6 benchmark: flat-array order book update cost.
#include "hft/book_view.hpp"
#include "hft/itch_message.hpp"
#include "hft/tsc.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
  hft::tsc_calibrate();
  const int n = (argc > 1) ? std::atoi(argv[1]) : 50'000'000;

  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 4096;

  // Pre-generate a stream so RNG cost isn't in the measured loop. A tight band
  // of ~64 ticks around mid models a realistic active top-of-book.
  std::vector<hft::ItchMessage> stream(static_cast<std::size_t>(n));
  std::uint64_t s = 0x1234;
  auto xs = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
  for (auto& m : stream) {
    const int off = 2000 + static_cast<int>(xs() % 64);
    m.price = base + static_cast<std::int64_t>(off) * tick;
    m.side = (xs() & 1) ? static_cast<std::uint8_t>(hft::Side::kBuy)
                        : static_cast<std::uint8_t>(hft::Side::kSell);
    const int roll = static_cast<int>(xs() % 10);
    m.type = roll < 6 ? static_cast<std::uint8_t>(hft::MsgType::kAdd)
                      : static_cast<std::uint8_t>(hft::MsgType::kExecute);
    m.size = static_cast<std::uint32_t>(1 + xs() % 500);
  }

  hft::BookView<N> book(base, tick);
  volatile std::int64_t sink = 0;

  const std::uint64_t t0 = hft::tsc_now_serialized();
  for (const auto& m : stream) {
    book.apply(m);
    sink += book.best_bid_size();  // force top-of-book to stay live
  }
  const std::uint64_t t1 = hft::tsc_now_serialized();
  (void)sink;

  const double ns = hft::tsc_to_ns(t1 - t0);
  std::printf("book apply(): %.2f ns/update  %.0f M updates/s  (out_of_range=%llu)\n",
              ns / n, static_cast<double>(n) / (ns / 1e9) / 1e6,
              static_cast<unsigned long long>(book.out_of_range()));
  return 0;
}
