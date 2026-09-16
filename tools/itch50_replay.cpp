// Replay a real Nasdaq TotalView-ITCH 5.0 file through the decoder and the
// order book, reporting message coverage and book invariants.
//
// This is the end-to-end check that the decoder works on production bytes
// rather than only on the fixtures in tests/test_itch50.cpp. It is a tool
// rather than a test because it needs a data file that is not committed --
// see scripts/fetch_itch_sample.py.
//
//   python3 scripts/fetch_itch_sample.py --mb 4
//   build/tools/itch50_replay data/itch_sample.bin
#include "hft/book_view.hpp"
#include "hft/itch50.hpp"
#include "hft/itch_message.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr std::size_t kTicks = 65536;  // cent ticks: covers $0 - $655.35

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <itch-binary-file>\n", argv[0]);
    std::fprintf(stderr, "get one with: python3 scripts/fetch_itch_sample.py\n");
    return 2;
  }

  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::byte> buf(static_cast<std::size_t>(size));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fprintf(stderr, "short read\n");
    std::fclose(f);
    return 1;
  }
  std::fclose(f);
  std::printf("replaying %s (%ld bytes)\n\n", argv[1], size);

  hft::itch50::BinaryFileReader reader(buf.data(), buf.size());
  hft::itch50::Decoder decoder;

  // One book for the single most active symbol would need a symbol map; this
  // tool is validating the decoder and the framing, so it applies every delta
  // to one book and only checks that the book stays internally coherent.
  hft::BookView<kTicks> book(0, hft::kPriceScale / 100);

  std::array<std::uint64_t, 128> by_type{};
  std::uint64_t messages = 0;
  std::uint64_t deltas = 0;
  std::uint64_t invariant_failures = 0;

  const std::byte* body = nullptr;
  std::size_t len = 0;
  while (reader.next(body, len)) {
    ++messages;
    const auto type = static_cast<unsigned char>(std::to_integer<std::uint8_t>(body[0]));
    if (type < by_type.size()) ++by_type[type];

    for (hft::itch50::LevelDelta d = decoder.apply(body, len); d.valid;
         d = decoder.take_pending()) {
      ++deltas;
      hft::ItchMessage m{};
      m.type = static_cast<std::uint8_t>(d.type);
      m.side = static_cast<std::uint8_t>(d.side);
      m.price = d.price;
      m.size = d.size;
      m.timestamp = d.timestamp_ns;
      book.apply(m);

      if (book.has_bid() && book.best_bid_size() == 0) ++invariant_failures;
      if (book.has_ask() && book.best_ask_size() == 0) ++invariant_failures;
      const double imb = book.imbalance();
      if (!(imb >= -1.0 && imb <= 1.0)) ++invariant_failures;
    }
  }

  const auto& s = decoder.stats();
  std::printf("messages framed      : %llu\n", static_cast<unsigned long long>(messages));
  std::printf("decoded              : %llu\n", static_cast<unsigned long long>(s.decoded));
  std::printf("ignored (other types): %llu\n", static_cast<unsigned long long>(s.ignored));
  std::printf("malformed length     : %llu\n", static_cast<unsigned long long>(s.malformed));
  std::printf("unknown order ref    : %llu\n", static_cast<unsigned long long>(s.unknown_order));
  std::printf("live orders at end   : %llu\n", static_cast<unsigned long long>(s.live_orders));
  std::printf("book level deltas    : %llu\n", static_cast<unsigned long long>(deltas));
  std::printf("out-of-window prices : %llu\n",
              static_cast<unsigned long long>(book.out_of_range()));
  std::printf("invariant failures   : %llu\n", static_cast<unsigned long long>(invariant_failures));

  std::printf("\nmessage type coverage:\n");
  std::printf("  %-6s %12s  %s\n", "type", "count", "decoded?");
  for (std::size_t i = 0; i < by_type.size(); ++i) {
    if (by_type[i] == 0) continue;
    const char c = static_cast<char>(i);
    const bool handled = hft::itch50::expected_length(c) != 0;
    std::printf("  %-6c %12llu  %s\n", c, static_cast<unsigned long long>(by_type[i]),
                handled ? "yes" : "no (ignored)");
  }
  return invariant_failures == 0 ? 0 : 1;
}
