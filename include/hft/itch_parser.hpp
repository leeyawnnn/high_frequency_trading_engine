// Zero-copy ITCH frame parsing.
//
// "Zero-copy" here means: no allocation, no tokenizing, no streams — a frame is
// turned into a fully-typed ItchMessage by copying its 32 bytes into a
// stack/register-resident struct. We use std::memcpy rather than a
// reinterpret_cast<const ItchMessage*> because the latter is strict-aliasing UB
// when the source buffer wasn't created as an ItchMessage and may be unaligned.
// At -O3 the memcpy of a trivially-copyable 32-byte POD lowers to the exact same
// handful of loads a cast would have produced — we pay nothing for the safety.
#pragma once

#include <cstddef>
#include <cstring>

#include "hft/compiler.hpp"
#include "hft/itch_message.hpp"

namespace hft {

// Decode one frame at `src` (must point to >= kMsgSize readable bytes).
HFT_ALWAYS_INLINE ItchMessage parse_message(const std::byte* src) noexcept {
  ItchMessage m;
  std::memcpy(&m, src, kMsgSize);
  return m;  // NRVO: stays in registers / on the caller's stack
}

// Encode `m` into `dst` (must point to >= kMsgSize writable bytes).
HFT_ALWAYS_INLINE void encode_message(const ItchMessage& m,
                                      std::byte* dst) noexcept {
  std::memcpy(dst, &m, kMsgSize);
}

// Walks a contiguous buffer of back-to-back fixed-size frames. Used by the feed
// handler to drain a datagram that carried several messages. Holds no storage
// of its own — it is a cursor over caller-owned bytes.
class FeedReader {
 public:
  FeedReader(const std::byte* data, std::size_t bytes) noexcept
      : cur_(data), end_(data + bytes) {}

  // True if at least one more whole frame remains.
  HFT_ALWAYS_INLINE bool has_next() const noexcept {
    return static_cast<std::size_t>(end_ - cur_) >= kMsgSize;
  }

  // Decode and advance. Precondition: has_next().
  HFT_ALWAYS_INLINE ItchMessage next() noexcept {
    ItchMessage m = parse_message(cur_);
    cur_ += kMsgSize;
    return m;
  }

  // Bytes left over that don't form a whole frame (a truncated tail).
  std::size_t remaining_bytes() const noexcept {
    return static_cast<std::size_t>(end_ - cur_);
  }

 private:
  const std::byte* cur_;
  const std::byte* end_;
};

}  // namespace hft
