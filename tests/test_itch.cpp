// Phase 3: ITCH-style binary feed + zero-copy parser.
#include "test_harness.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

bool bitwise_equal(const hft::ItchMessage& a, const hft::ItchMessage& b) {
  return std::memcmp(&a, &b, hft::kMsgSize) == 0;
}

hft::ItchMessage make_message(std::mt19937_64& rng, std::uint32_t seq) {
  static const char* syms[] = {"AAPL", "MSFT", "TST", "ES", "NVDA"};
  static const std::uint8_t types[] = {'A', 'E', 'X', 'D', 'P'};
  hft::ItchMessage m{};
  m.timestamp = rng();
  m.price = static_cast<std::int64_t>(rng() % 5'000'000ULL) + 1;
  m.symbol = hft::pack_symbol(syms[rng() % 5]);
  m.size = static_cast<std::uint32_t>(rng() % 100'000ULL);
  m.seq = seq;
  m.type = types[rng() % 5];
  m.side = (rng() & 1) ? static_cast<std::uint8_t>('B')
                       : static_cast<std::uint8_t>('S');
  m.flags = static_cast<std::uint16_t>(rng() & 0xFFFF);
  return m;
}

}  // namespace

HFT_TEST(format_is_locked) {
  CHECK_EQ(sizeof(hft::ItchMessage), 32u);
  CHECK_EQ(hft::kMsgSize, 32u);
}

HFT_TEST(symbol_roundtrip) {
  for (const char* s : {"AAPL", "MS", "X", "NVDA", ""}) {
    char out[5];
    hft::unpack_symbol(hft::pack_symbol(s), out);
    CHECK(std::strcmp(out, s) == 0);
  }
}

HFT_TEST(price_fixed_point_roundtrip) {
  CHECK_EQ(hft::price_from_double(123.4500), 1'234'500);
  CHECK_NEAR(hft::price_to_double(1'234'500), 123.45, 1e-9);
  // Round-trip a range of prices through fixed point.
  for (double px = 0.0001; px < 1000.0; px += 7.3137) {
    const std::int64_t fx = hft::price_from_double(px);
    CHECK_NEAR(hft::price_to_double(fx), px, 1e-4);
  }
}

HFT_TEST(single_message_roundtrip) {
  hft::ItchMessage m{};
  m.timestamp = 0xDEADBEEFCAFEF00DULL;
  m.price = hft::price_from_double(42.25);
  m.symbol = hft::pack_symbol("TST");
  m.size = 12345;
  m.seq = 7;
  m.type = static_cast<std::uint8_t>(hft::MsgType::kAdd);
  m.side = static_cast<std::uint8_t>(hft::Side::kBuy);
  m.flags = 0xBEEF;

  std::byte buf[hft::kMsgSize];
  hft::encode_message(m, buf);
  const hft::ItchMessage parsed = hft::parse_message(buf);

  CHECK(bitwise_equal(m, parsed));
  CHECK_EQ(parsed.size, 12345u);
  CHECK(hft::msg_type(parsed) == hft::MsgType::kAdd);
  CHECK(hft::msg_side(parsed) == hft::Side::kBuy);
}

HFT_TEST(stream_100k_byte_perfect) {
  constexpr int kN = 100'000;
  std::mt19937_64 rng(0xA11CE);

  // Build the reference messages and serialize them back-to-back into one
  // contiguous byte buffer (exactly what a datagram from exchange_sim looks
  // like for a multi-message packet).
  std::vector<hft::ItchMessage> ref;
  ref.reserve(kN);
  std::vector<std::byte> wire(static_cast<std::size_t>(kN) * hft::kMsgSize);

  for (int i = 0; i < kN; ++i) {
    hft::ItchMessage m = make_message(rng, static_cast<std::uint32_t>(i));
    ref.push_back(m);
    hft::encode_message(m, wire.data() + static_cast<std::size_t>(i) * hft::kMsgSize);
  }

  // Drain with the streaming reader; every frame must be byte-identical and in
  // order, with the sequence numbers contiguous (no gaps).
  hft::FeedReader reader(wire.data(), wire.size());
  int count = 0;
  std::uint32_t expected_seq = 0;
  while (reader.has_next()) {
    const hft::ItchMessage got = reader.next();
    CHECK(bitwise_equal(ref[static_cast<std::size_t>(count)], got));
    CHECK_EQ(got.seq, expected_seq);
    ++expected_seq;
    ++count;
  }
  CHECK_EQ(count, kN);
  CHECK_EQ(reader.remaining_bytes(), 0u);
}

HFT_TEST(reader_handles_truncated_tail) {
  // A datagram that ends mid-frame: reader must stop cleanly and report the
  // leftover bytes rather than read past the end.
  std::vector<std::byte> wire(hft::kMsgSize + 10);
  hft::ItchMessage m{};
  m.seq = 99;
  hft::encode_message(m, wire.data());
  hft::FeedReader reader(wire.data(), wire.size());
  CHECK(reader.has_next());
  (void)reader.next();
  CHECK(!reader.has_next());
  CHECK_EQ(reader.remaining_bytes(), 10u);
}

HFT_TEST_MAIN()
