// libFuzzer target: feed arbitrary bytes to the book as a message stream and
// assert its invariants after every message.
//
// The differential and property tests in tests/test_book.cpp drive the book
// with generators I wrote, so they explore the input space I thought of. This
// explores the one I did not: the fuzzer mutates raw bytes into the wire
// struct, producing message types, sides, sizes and prices that no generator
// here would emit -- reserved type bytes, sizes at the width of the field,
// prices at the extremes of int64.
//
// Build:
//   cmake -S . -B build-fuzz -DHFT_BUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
//         -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_book
//   ./build-fuzz/fuzz/fuzz_book -max_total_time=60
//
// A crash writes its input to crash-<sha1>; replay with
//   ./build-fuzz/fuzz/fuzz_book crash-<sha1>
#include "hft/book_view.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t kTicks = 1024;

// Abort rather than return on violation: libFuzzer reports the crashing input,
// which is the whole point. assert() would vanish under NDEBUG, so this is
// deliberately not an assert.
void require(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "book invariant violated: %s\n", what);
    __builtin_trap();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<kTicks> book(base, tick);

  const std::size_t count = size / hft::kMsgSize;
  for (std::size_t i = 0; i < count; ++i) {
    const hft::ItchMessage m =
        hft::parse_message(reinterpret_cast<const std::byte*>(data) + i * hft::kMsgSize);
    book.apply(m);

    // A side that reports a best price must have size there.
    if (book.has_bid()) {
      require(book.best_bid_size() > 0, "best bid has zero size");
      require(book.size_at(hft::Side::kBuy, book.best_bid_price()) == book.best_bid_size(),
              "best bid size disagrees with size_at");
    }
    if (book.has_ask()) {
      require(book.best_ask_size() > 0, "best ask has zero size");
      require(book.size_at(hft::Side::kSell, book.best_ask_price()) == book.best_ask_size(),
              "best ask size disagrees with size_at");
    }

    // Imbalance is a ratio of non-negative sizes.
    const double imb = book.imbalance();
    require(imb >= -1.0 && imb <= 1.0, "imbalance outside [-1, 1]");

    // A reported best price must lie inside the window the book covers.
    if (book.has_bid()) {
      const std::int64_t p = book.best_bid_price();
      require(p >= base && p < base + static_cast<std::int64_t>(kTicks) * tick,
              "best bid price outside the book window");
    }
    if (book.has_ask()) {
      const std::int64_t p = book.best_ask_price();
      require(p >= base && p < base + static_cast<std::int64_t>(kTicks) * tick,
              "best ask price outside the book window");
    }
  }
  return 0;
}
