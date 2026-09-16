// Conformance tests for the Nasdaq TotalView-ITCH 5.0 decoder.
//
// These build messages byte-by-byte to the spec's layout and assert the
// decoder recovers the fields. Writing the encoder separately from the decoder
// is the point: if both shared a struct definition the test would only prove
// the struct round-trips itself, which is what the simplified internal format
// already does and is not what "conformant" means.
//
// The field offsets asserted here were cross-checked against real data --
// 01302019.NASDAQ_ITCH50 from Nasdaq's sample archive -- before this file was
// written. scripts/fetch_itch_sample.py reproduces that check on demand; it is
// not run in CI because the source file is 4.76 GB and the data is Nasdaq's to
// distribute, not this repository's.
#include "hft/itch50.hpp"
#include "hft/itch_message.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace hft::itch50;

// ---- a spec-literal encoder, written from the spec tables -------------------
struct Builder {
  std::vector<std::byte> bytes;

  void u8(std::uint8_t v) { bytes.push_back(std::byte{v}); }
  void ch(char c) { u8(static_cast<std::uint8_t>(c)); }
  void be16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v >> 8));
    u8(static_cast<std::uint8_t>(v & 0xFF));
  }
  void be32(std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) u8(static_cast<std::uint8_t>((v >> s) & 0xFF));
  }
  void be48(std::uint64_t v) {
    for (int s = 40; s >= 0; s -= 8) u8(static_cast<std::uint8_t>((v >> s) & 0xFF));
  }
  void be64(std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) u8(static_cast<std::uint8_t>((v >> s) & 0xFF));
  }
  void stock(const char* s) {
    // Spec: 8 bytes, left-justified, space-padded.
    for (int i = 0; i < 8; ++i) ch(s[i] && i < 8 ? (s[i] ? s[i] : ' ') : ' ');
  }
  void header(char type, std::uint16_t locate, std::uint16_t track, std::uint64_t ts) {
    ch(type);
    be16(locate);
    be16(track);
    be48(ts);
  }
  const std::byte* data() const { return bytes.data(); }
  std::size_t size() const { return bytes.size(); }
};

Builder add_order(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price,
                  std::uint64_t ts = 34'200'000'000'000ULL) {
  Builder b;
  b.header('A', 42, 0, ts);
  b.be64(ref);
  b.ch(side);
  b.be32(shares);
  b.stock("AAPL    ");
  b.be32(price);
  return b;
}

}  // namespace

HFT_TEST(spec_message_lengths_match_the_published_table) {
  // Each of these was also observed in the sample file; a mismatch means the
  // framing assumption is wrong and every field offset after it is garbage.
  CHECK_EQ(expected_length('S'), 12u);
  CHECK_EQ(expected_length('R'), 39u);
  CHECK_EQ(expected_length('H'), 25u);
  CHECK_EQ(expected_length('Y'), 20u);
  CHECK_EQ(expected_length('L'), 26u);
  CHECK_EQ(expected_length('A'), 36u);
  CHECK_EQ(expected_length('F'), 40u);
  CHECK_EQ(expected_length('E'), 31u);
  CHECK_EQ(expected_length('C'), 36u);
  CHECK_EQ(expected_length('X'), 23u);
  CHECK_EQ(expected_length('D'), 19u);
  CHECK_EQ(expected_length('U'), 35u);
  CHECK_EQ(expected_length('P'), 44u);
  // A type we deliberately do not decode.
  CHECK_EQ(expected_length('Q'), 0u);
}

HFT_TEST(big_endian_readers) {
  const std::byte buf[8] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                            std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
  CHECK_EQ(hft::itch50::be16(buf), 0x0102u);
  CHECK_EQ(hft::itch50::be32(buf), 0x01020304u);
  CHECK_EQ(hft::itch50::be48(buf), 0x010203040506ULL);
  CHECK_EQ(hft::itch50::be64(buf), 0x0102030405060708ULL);
}

HFT_TEST(header_decodes_at_spec_offsets) {
  Builder b;
  const std::uint64_t ts = 9ULL * 3600 * 1'000'000'000 + 30ULL * 60 * 1'000'000'000;  // 09:30:00
  b.header('S', 0x1234, 0x5678, ts);
  b.ch('Q');  // start of market hours
  CHECK_EQ(b.size(), kLenSystemEvent);

  const Header h = read_header(b.data());
  CHECK_EQ(h.type, 'S');
  CHECK_EQ(h.stock_locate, 0x1234u);
  CHECK_EQ(h.tracking_number, 0x5678u);
  CHECK_EQ(h.timestamp_ns, ts);
}

HFT_TEST(add_order_decodes_to_a_level_delta) {
  Decoder d;
  // 1,000 shares at 107.2600 -> ITCH price 1072600 (four implied decimals).
  const Builder b = add_order(9005, 'S', 1000, 1'072'600);
  CHECK_EQ(b.size(), kLenAddOrder);

  const LevelDelta delta = d.apply(b.data(), b.size());
  CHECK(delta.valid);
  CHECK(delta.type == hft::MsgType::kAdd);
  CHECK(delta.side == hft::Side::kSell);
  CHECK_EQ(delta.size, 1000u);
  CHECK_EQ(delta.price, hft::price_from_double(107.26));
  CHECK_EQ(d.stats().decoded, 1u);
  CHECK_EQ(d.stats().live_orders, 1u);
}

HFT_TEST(price_conversion_matches_four_implied_decimals) {
  CHECK_EQ(Decoder::to_engine_price(1'072'600), hft::price_from_double(107.26));
  CHECK_EQ(Decoder::to_engine_price(391'600), hft::price_from_double(39.16));
  CHECK_EQ(Decoder::to_engine_price(10'000), hft::price_from_double(1.00));
  CHECK_EQ(Decoder::to_engine_price(1), hft::price_from_double(0.0001));
}

HFT_TEST(execute_and_cancel_resolve_the_order_reference) {
  // The substantive difference from the simplified format: these messages
  // carry only a reference, so the decoder must remember the Add.
  Decoder d;
  const Builder a = add_order(777, 'B', 500, 500'000);  // 50.00
  (void)d.apply(a.data(), a.size());

  Builder e;
  e.header('E', 42, 0, 34'200'000'000'001ULL);
  e.be64(777);
  e.be32(200);     // executed shares
  e.be64(123456);  // match number
  CHECK_EQ(e.size(), kLenOrderExecuted);

  const LevelDelta ex = d.apply(e.data(), e.size());
  CHECK(ex.valid);
  CHECK(ex.type == hft::MsgType::kExecute);
  CHECK(ex.side == hft::Side::kBuy);                  // recovered from the Add
  CHECK_EQ(ex.price, hft::price_from_double(50.00));  // recovered from the Add
  CHECK_EQ(ex.size, 200u);

  Builder x;
  x.header('X', 42, 0, 34'200'000'000'002ULL);
  x.be64(777);
  x.be32(100);
  CHECK_EQ(x.size(), kLenOrderCancel);

  const LevelDelta cx = d.apply(x.data(), x.size());
  CHECK(cx.valid);
  CHECK(cx.type == hft::MsgType::kCancel);
  CHECK_EQ(cx.size, 100u);
  CHECK_EQ(d.stats().live_orders, 1u);  // 200 shares remain
}

HFT_TEST(delete_removes_exactly_the_remaining_shares) {
  Decoder d;
  const Builder a = add_order(31337, 'B', 900, 250'000);
  (void)d.apply(a.data(), a.size());

  Builder e;
  e.header('E', 42, 0, 1);
  e.be64(31337);
  e.be32(400);
  e.be64(1);
  (void)d.apply(e.data(), e.size());  // 500 remain

  Builder del;
  del.header('D', 42, 0, 2);
  del.be64(31337);
  CHECK_EQ(del.size(), kLenOrderDelete);

  const LevelDelta dd = d.apply(del.data(), del.size());
  CHECK(dd.valid);
  CHECK_EQ(dd.size, 500u);  // the remainder, not the original 900
  CHECK_EQ(d.stats().live_orders, 0u);
}

HFT_TEST(replace_produces_two_level_deltas) {
  Decoder d;
  const Builder a = add_order(1000, 'B', 300, 100'000);  // 10.00
  (void)d.apply(a.data(), a.size());

  Builder u;
  u.header('U', 42, 0, 3);
  u.be64(1000);     // original reference
  u.be64(2000);     // new reference
  u.be32(450);      // new shares
  u.be32(110'000);  // new price, 11.00
  CHECK_EQ(u.size(), kLenOrderReplace);

  const LevelDelta removal = d.apply(u.data(), u.size());
  CHECK(removal.valid);
  CHECK(removal.type == hft::MsgType::kCancel);
  CHECK_EQ(removal.price, hft::price_from_double(10.00));
  CHECK_EQ(removal.size, 300u);

  const LevelDelta insertion = d.take_pending();
  CHECK(insertion.valid);
  CHECK(insertion.type == hft::MsgType::kAdd);
  CHECK(insertion.side == hft::Side::kBuy);  // side carried over from the original
  CHECK_EQ(insertion.price, hft::price_from_double(11.00));
  CHECK_EQ(insertion.size, 450u);

  // The pending delta is consumed exactly once.
  CHECK(!d.take_pending().valid);
}

HFT_TEST(unknown_order_reference_is_counted_not_guessed) {
  Decoder d;
  Builder del;
  del.header('D', 42, 0, 1);
  del.be64(999999);  // never added
  const LevelDelta out = d.apply(del.data(), del.size());
  CHECK(!out.valid);
  CHECK_EQ(d.stats().unknown_order, 1u);
}

HFT_TEST(wrong_length_is_rejected_rather_than_decoded) {
  // A body whose length disagrees with the spec means the stream is misframed.
  // Decoding it anyway yields plausible-looking garbage, which is worse than
  // refusing.
  Decoder d;
  Builder b = add_order(5, 'B', 100, 10'000);
  b.bytes.pop_back();  // 35 bytes, spec says 36
  const LevelDelta out = d.apply(b.data(), b.size());
  CHECK(!out.valid);
  CHECK_EQ(d.stats().malformed, 1u);
  CHECK_EQ(d.stats().decoded, 0u);
}

HFT_TEST(trade_message_does_not_touch_the_book) {
  // 'P' is a non-displayable execution: it prints to the tape but was never
  // resting liquidity, so it must not produce a level delta.
  Decoder d;
  Builder p;
  p.header('P', 42, 0, 1);
  p.be64(0);
  p.ch('B');
  p.be32(1);
  p.stock("UGAZ    ");
  p.be32(391'600);
  p.be64(42);
  CHECK_EQ(p.size(), kLenTrade);

  const LevelDelta out = d.apply(p.data(), p.size());
  CHECK(!out.valid);
  CHECK_EQ(d.stats().decoded, 1u);  // decoded, but not a book change
}

HFT_TEST(binary_file_framing_and_truncation) {
  // Nasdaq's day files prefix each message with a 2-byte big-endian length.
  Builder stream;
  const Builder a1 = add_order(1, 'B', 100, 10'000);
  const Builder a2 = add_order(2, 'S', 200, 20'000);
  stream.be16(static_cast<std::uint16_t>(a1.size()));
  stream.bytes.insert(stream.bytes.end(), a1.bytes.begin(), a1.bytes.end());
  stream.be16(static_cast<std::uint16_t>(a2.size()));
  stream.bytes.insert(stream.bytes.end(), a2.bytes.begin(), a2.bytes.end());

  BinaryFileReader reader(stream.data(), stream.size());
  const std::byte* body = nullptr;
  std::size_t len = 0;
  int count = 0;
  Decoder d;
  while (reader.next(body, len)) {
    const LevelDelta delta = d.apply(body, len);
    CHECK(delta.valid);
    ++count;
  }
  CHECK_EQ(count, 2);

  // A truncated final message must be refused, not partially decoded.
  BinaryFileReader truncated(stream.data(), stream.size() - 5);
  int seen = 0;
  while (truncated.next(body, len)) ++seen;
  CHECK_EQ(seen, 1);
}

HFT_TEST_MAIN()
