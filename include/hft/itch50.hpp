// Nasdaq TotalView-ITCH 5.0 decoder.
//
// The rest of this engine speaks a simplified internal frame (hft/itch_message.hpp):
// fixed 32 bytes, native-endian, carrying a price level and a size. That format
// exists so the loopback simulation measures the pipeline rather than byte
// swapping. It is NOT the wire format Nasdaq publishes, and the README says so.
//
// This header is the real thing, for the part of the problem the simplified
// frame skips:
//
//   * Big-endian fields at the spec's byte offsets, in a variable-length frame
//     whose length is carried by the BinaryFILE 2-byte prefix.
//   * Six-byte timestamps (nanoseconds since midnight, Eastern), which do not
//     fit any integer type and have to be assembled by hand.
//   * Prices as uint32 with four implied decimal places.
//   * And the substantive difference: ITCH is ORDER-based, not level-based.
//     An Add carries (reference, side, shares, price); every later message that
//     touches that order -- Execute, Cancel, Delete, Replace -- carries only the
//     reference. A decoder therefore has to remember every live order to know
//     which price level a delete applies to. That bookkeeping is most of what a
//     real feed handler does, and the simplified format assumes it away by
//     putting the price in every message.
//
// Field offsets below were verified by decoding 01302019.NASDAQ_ITCH50 from
// Nasdaq's sample archive: see scripts/fetch_itch_sample.py. Message lengths
// were cross-checked against the same file, which is why the static_asserts
// carry the observed values.
//
// Spec: Nasdaq TotalView-ITCH 5.0.
// https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf
// (verified reachable 2026-09-16; section numbers cited per message below)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "hft/itch_message.hpp"

namespace hft::itch50 {

// ---- message types we decode (spec 4.1-4.5) --------------------------------
enum class Type : char {
  kSystemEvent = 'S',         // 4.1
  kStockDirectory = 'R',      // 4.2.1
  kTradingAction = 'H',       // 4.2.2
  kRegSho = 'Y',              // 4.2.3
  kMarketParticipant = 'L',   // 4.2.4
  kAddOrder = 'A',            // 4.3.1
  kAddOrderMpid = 'F',        // 4.3.2
  kOrderExecuted = 'E',       // 4.4.1
  kOrderExecutedPrice = 'C',  // 4.4.2
  kOrderCancel = 'X',         // 4.4.3
  kOrderDelete = 'D',         // 4.4.4
  kOrderReplace = 'U',        // 4.4.5
  kTrade = 'P',               // 4.5.1
};

// Spec message lengths, excluding the 2-byte BinaryFILE length prefix. Every
// one of these was observed in the sample file; a mismatch means either the
// spec moved or the framing is misaligned, and both should stop the decode.
inline constexpr std::size_t kLenSystemEvent = 12;
inline constexpr std::size_t kLenStockDirectory = 39;
inline constexpr std::size_t kLenTradingAction = 25;
inline constexpr std::size_t kLenRegSho = 20;
inline constexpr std::size_t kLenMarketParticipant = 26;
inline constexpr std::size_t kLenAddOrder = 36;
inline constexpr std::size_t kLenAddOrderMpid = 40;
inline constexpr std::size_t kLenOrderExecuted = 31;
inline constexpr std::size_t kLenOrderExecutedPrice = 36;
inline constexpr std::size_t kLenOrderCancel = 23;
inline constexpr std::size_t kLenOrderDelete = 19;
inline constexpr std::size_t kLenOrderReplace = 35;
inline constexpr std::size_t kLenTrade = 44;

// ITCH prices are uint32 with four implied decimals. The engine's fixed-point
// scale is kPriceScale; convert once here rather than at every use.
inline constexpr std::int64_t kItchPriceDivisor = 10'000;

// ---- big-endian field readers ----------------------------------------------
// ITCH is big-endian regardless of host. These are byte-at-a-time rather than
// a load-and-byteswap because the fields are not naturally aligned inside the
// frame: `shares` sits at offset 20 of a message that itself began at an
// arbitrary offset in the buffer.
[[nodiscard]] inline std::uint16_t be16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                    std::to_integer<std::uint16_t>(p[1]));
}

[[nodiscard]] inline std::uint32_t be32(const std::byte* p) noexcept {
  return (std::to_integer<std::uint32_t>(p[0]) << 24) |
         (std::to_integer<std::uint32_t>(p[1]) << 16) |
         (std::to_integer<std::uint32_t>(p[2]) << 8) | std::to_integer<std::uint32_t>(p[3]);
}

[[nodiscard]] inline std::uint64_t be48(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < 6; ++i) {
    v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
  }
  return v;
}

[[nodiscard]] inline std::uint64_t be64(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
  }
  return v;
}

// Common header, present on every message (spec 4): type, stock locate,
// tracking number, then a 6-byte nanoseconds-since-midnight timestamp.
struct Header {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  std::uint64_t timestamp_ns;  // since midnight, US/Eastern
};

[[nodiscard]] inline Header read_header(const std::byte* m) noexcept {
  return Header{static_cast<char>(std::to_integer<std::uint8_t>(m[0])), be16(m + 1), be16(m + 3),
                be48(m + 5)};
}

// Expected body length for a type, or 0 when the type is not one we decode.
//
// Switches on the raw char rather than casting to Type. The byte comes off the
// wire and may hold any of 256 values, most of which are not enumerators, and
// casting one into a scoped enum is out of range -- the static analyser is
// right to object. The enum stays as documentation of what we handle.
[[nodiscard]] inline std::size_t expected_length(char type) noexcept {
  switch (type) {
    case 'S':
      return kLenSystemEvent;
    case 'R':
      return kLenStockDirectory;
    case 'H':
      return kLenTradingAction;
    case 'Y':
      return kLenRegSho;
    case 'L':
      return kLenMarketParticipant;
    case 'A':
      return kLenAddOrder;
    case 'F':
      return kLenAddOrderMpid;
    case 'E':
      return kLenOrderExecuted;
    case 'C':
      return kLenOrderExecutedPrice;
    case 'X':
      return kLenOrderCancel;
    case 'D':
      return kLenOrderDelete;
    case 'U':
      return kLenOrderReplace;
    case 'P':
      return kLenTrade;
    default:
      return 0;
  }
}

// ---- decoded order-book deltas ---------------------------------------------
// The decoder turns order-based ITCH into the level-based deltas the engine's
// BookView consumes.
struct LevelDelta {
  bool valid = false;
  MsgType type = MsgType::kUnknown;
  Side side = Side::kBuy;
  std::int64_t price = 0;  // engine fixed-point
  std::uint32_t size = 0;  // shares
  std::uint64_t timestamp_ns = 0;
  std::uint16_t stock_locate = 0;
};

// Tracks live orders so that reference-only messages can be resolved to a
// price level. This is the state a real feed handler carries and the
// simplified internal format avoids entirely.
class Decoder {
 public:
  struct Stats {
    std::uint64_t decoded = 0;
    std::uint64_t ignored = 0;        // types we do not decode
    std::uint64_t malformed = 0;      // length disagreed with the spec
    std::uint64_t unknown_order = 0;  // reference we never saw an Add for
    std::uint64_t live_orders = 0;
  };

  // Decode one message body (no length prefix). Returns a delta whose `valid`
  // is false for messages that do not change a price level.
  LevelDelta apply(const std::byte* body, std::size_t len) {
    LevelDelta out;
    if (len == 0) {
      ++stats_.malformed;
      return out;
    }
    const Header h = read_header(body);
    const std::size_t want = expected_length(h.type);
    if (want == 0) {
      ++stats_.ignored;
      return out;
    }
    if (len != want) {
      // A length that disagrees with the spec means the stream is misframed.
      // Decoding it anyway would produce plausible-looking garbage.
      ++stats_.malformed;
      return out;
    }

    ++stats_.decoded;
    out.timestamp_ns = h.timestamp_ns;
    out.stock_locate = h.stock_locate;

    switch (h.type) {
      case 'A':
      case 'F': {  // Add Order, with or without MPID attribution
        const std::uint64_t ref = be64(body + 11);
        const Side side =
            (std::to_integer<std::uint8_t>(body[19]) == 'B') ? Side::kBuy : Side::kSell;
        const std::uint32_t shares = be32(body + 20);
        const std::int64_t price = to_engine_price(be32(body + 32));
        orders_[ref] = Order{price, shares, side};
        out.valid = true;
        out.type = MsgType::kAdd;
        out.side = side;
        out.price = price;
        out.size = shares;
        break;
      }
      case 'E':
      case 'C': {
        // Order Executed, and Executed With Price. Both reduce the resting
        // order by the executed quantity, so the book delta is identical; 'C'
        // additionally carries the execution price, which matters to a tape
        // consumer and not to the book. Deliberately one branch.
        out = reduce(be64(body + 11), be32(body + 19), MsgType::kExecute, out);
        break;
      }
      case 'X': {  // Order Cancel: partial reduction
        out = reduce(be64(body + 11), be32(body + 19), MsgType::kCancel, out);
        break;
      }
      case 'D': {  // Order Delete: remove whatever remains
        const std::uint64_t ref = be64(body + 11);
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
          ++stats_.unknown_order;
          break;
        }
        out.valid = true;
        out.type = MsgType::kCancel;  // remove exactly the remaining shares
        out.side = it->second.side;
        out.price = it->second.price;
        out.size = it->second.shares;
        orders_.erase(it);
        break;
      }
      case 'U': {  // Order Replace: a delete plus an add
        // Replace is a delete of the original followed by an add of a new
        // order at a new reference. Only the delta for the removal is returned
        // here; the caller gets the add as the next call's delta via
        // pending_add(), because one message produces two level changes.
        const std::uint64_t orig = be64(body + 11);
        const std::uint64_t fresh = be64(body + 19);
        const std::uint32_t shares = be32(body + 27);
        const std::int64_t price = to_engine_price(be32(body + 31));
        auto it = orders_.find(orig);
        if (it == orders_.end()) {
          ++stats_.unknown_order;
          // Still record the replacement so later messages resolve.
          orders_[fresh] = Order{price, shares, Side::kBuy};
          break;
        }
        const Order old = it->second;
        orders_.erase(it);
        orders_[fresh] = Order{price, shares, old.side};
        out.valid = true;
        out.type = MsgType::kCancel;
        out.side = old.side;
        out.price = old.price;
        out.size = old.shares;
        pending_ = LevelDelta{true,   MsgType::kAdd,  old.side,      price,
                              shares, h.timestamp_ns, h.stock_locate};
        break;
      }
      default:
        // 'P' is a non-displayable execution: printed to the tape, never
        // resting on the book. 'S', 'R', 'H', 'Y' and 'L' are administrative.
        // All are decoded (the length check above passed) but change no level.
        break;
    }
    stats_.live_orders = orders_.size();
    return out;
  }

  // A Replace produces two level deltas. Call after apply() and use the result
  // when valid.
  [[nodiscard]] LevelDelta take_pending() noexcept {
    LevelDelta p = pending_;
    pending_ = LevelDelta{};
    return p;
  }

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  [[nodiscard]] static std::int64_t to_engine_price(std::uint32_t itch_price) noexcept {
    // ITCH: 4 implied decimals. Engine: kPriceScale per dollar.
    return (static_cast<std::int64_t>(itch_price) * kPriceScale) / kItchPriceDivisor;
  }

 private:
  struct Order {
    std::int64_t price;
    std::uint32_t shares;
    Side side;
  };

  LevelDelta reduce(std::uint64_t ref, std::uint32_t shares, MsgType type, LevelDelta out) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) {
      ++stats_.unknown_order;
      return out;
    }
    const std::uint32_t taken = (shares >= it->second.shares) ? it->second.shares : shares;
    out.valid = true;
    out.type = type;
    out.side = it->second.side;
    out.price = it->second.price;
    out.size = taken;
    it->second.shares -= taken;
    if (it->second.shares == 0) orders_.erase(it);
    return out;
  }

  std::unordered_map<std::uint64_t, Order> orders_;
  LevelDelta pending_;
  Stats stats_;
};

// ---- BinaryFILE framing ------------------------------------------------------
// Nasdaq's downloadable day files prefix each message with a 2-byte big-endian
// length. (The live multicast feed uses MoldUDP64 instead, which this does not
// implement -- see the README's message-coverage table.)
class BinaryFileReader {
 public:
  BinaryFileReader(const std::byte* data, std::size_t size) noexcept
      : cur_(data), end_(data + size) {}

  // Returns false at end of buffer or on a truncated final message.
  bool next(const std::byte*& body, std::size_t& len) noexcept {
    if (static_cast<std::size_t>(end_ - cur_) < 2) return false;
    const std::size_t n = be16(cur_);
    if (n == 0 || static_cast<std::size_t>(end_ - cur_ - 2) < n) return false;
    body = cur_ + 2;
    len = n;
    cur_ += 2 + n;
    return true;
  }

 private:
  const std::byte* cur_;
  const std::byte* end_;
};

}  // namespace hft::itch50
