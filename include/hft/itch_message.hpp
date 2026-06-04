// Compact ITCH-style binary market-data message (fixed 32 bytes).
//
// Why padding/alignment matters here — the central design choice:
//
//   * The struct is PACKED (#pragma pack(1)) so the layout is byte-exact and
//     deterministic across compilers/ABIs. That is what makes it a *wire*
//     format: the bytes the exchange_sim writes are exactly the bytes the feed
//     handler reads, with no compiler-inserted padding to disagree about.
//
//   * BUT the fields are deliberately ordered widest-first so that, even though
//     packing is on, every field still lands on its natural alignment within
//     the 32-byte frame (u64 @0,@8; u32 @16,@20,@24; u8 @28,@29; u16 @30).
//     So we get a deterministic packed layout WITHOUT paying for misaligned
//     loads on the hot path. If we'd put `type` (1 byte) first, the u64s would
//     straddle alignment boundaries and every access would risk a split load.
//
//   * 32 bytes is exactly half a cache line: two messages per line, and an
//     aligned message never straddles a line boundary.
//
// Real Nasdaq ITCH is big-endian and variable-length with a MoldUDP framing
// layer. We use native-endian fixed-size frames because this is a single-machine
// loopback simulation (producer and consumer share endianness) and byte-swaps
// would just add latency we're trying to measure. The framing simplification is
// called out in the README.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hft {

// ---- field encodings --------------------------------------------------------

enum class MsgType : std::uint8_t {
  kUnknown = 0,
  kAdd     = 'A',  // add liquidity at (side, price): resting size += size
  kExecute = 'E',  // trade against resting liquidity: resting size -= size
  kCancel  = 'X',  // partial cancel: resting size -= size
  kDelete  = 'D',  // remove the whole level at (side, price)
  kTrade   = 'P',  // print / last-sale (informational)
};

enum class Side : std::uint8_t {
  kNone = 0,
  kBuy  = 'B',
  kSell = 'S',
};

// Prices are fixed-point integers in units of 1/10000 of the quote currency
// (4 implied decimals), so $123.4500 -> 1'234'500. Integer math only on the
// hot path; conversion to double is a reporting-only convenience.
inline constexpr std::int64_t kPriceScale = 10'000;

inline constexpr std::int64_t price_from_double(double px) noexcept {
  return static_cast<std::int64_t>(px * static_cast<double>(kPriceScale) +
                                   (px < 0 ? -0.5 : 0.5));
}
inline constexpr double price_to_double(std::int64_t px) noexcept {
  return static_cast<double>(px) / static_cast<double>(kPriceScale);
}

// Pack up to 4 ASCII characters (e.g. a ticker) into a u32. Stored in the
// order given, NUL-padded. Symmetric with unpack_symbol().
inline std::uint32_t pack_symbol(const char* s) noexcept {
  char b[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4 && s[i] != '\0'; ++i) b[i] = s[i];
  std::uint32_t v;
  std::memcpy(&v, b, 4);
  return v;
}
inline void unpack_symbol(std::uint32_t sym, char out[5]) noexcept {
  std::memcpy(out, &sym, 4);
  out[4] = '\0';
}

// ---- the wire message -------------------------------------------------------

#pragma pack(push, 1)
struct ItchMessage {
  std::uint64_t timestamp;  // @0  exchange send stamp (TSC cycles or ns)
  std::int64_t  price;      // @8  fixed-point (see kPriceScale)
  std::uint32_t symbol;     // @16 packed 4-char ticker
  std::uint32_t size;       // @20 shares
  std::uint32_t seq;        // @24 monotonic sequence number (gap detection)
  std::uint8_t  type;       // @28 MsgType
  std::uint8_t  side;       // @29 Side
  std::uint16_t flags;      // @30 reserved / message flags
};
#pragma pack(pop)

inline constexpr std::size_t kMsgSize = 32;

// Wire-layout contract: any change here breaks compatibility with persisted
// captures and the exchange_sim, so it is locked down at compile time.
static_assert(sizeof(ItchMessage) == kMsgSize, "ItchMessage must be 32 bytes");
static_assert(alignof(ItchMessage) == 1, "packed message must be 1-aligned");
static_assert(offsetof(ItchMessage, timestamp) == 0);
static_assert(offsetof(ItchMessage, price) == 8);
static_assert(offsetof(ItchMessage, symbol) == 16);
static_assert(offsetof(ItchMessage, size) == 20);
static_assert(offsetof(ItchMessage, seq) == 24);
static_assert(offsetof(ItchMessage, type) == 28);
static_assert(offsetof(ItchMessage, side) == 29);
static_assert(offsetof(ItchMessage, flags) == 30);
// Even though packed, the widest fields remain naturally aligned (offsets are
// multiples of their size) — that is the whole point of the field ordering.
static_assert(offsetof(ItchMessage, timestamp) % 8 == 0);
static_assert(offsetof(ItchMessage, price) % 8 == 0);
static_assert(offsetof(ItchMessage, symbol) % 4 == 0);

inline MsgType msg_type(const ItchMessage& m) noexcept {
  return static_cast<MsgType>(m.type);
}
inline Side msg_side(const ItchMessage& m) noexcept {
  return static_cast<Side>(m.side);
}

}  // namespace hft
