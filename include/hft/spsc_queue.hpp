// Lock-free single-producer / single-consumer ring buffer.
//
// This is the backbone of the pipeline: feed-handler -> strategy and
// strategy -> gateway are each one of these, crossing a core boundary with no
// locks and no syscalls.
//
// Design points (all of which matter for latency):
//
//  * Power-of-two capacity → index wrap is a single AND, never a modulo.
//
//  * Monotonically increasing 64-bit positions (never wrapped). Occupancy is
//    just (producer_pos - consumer_pos); empty/full need no spare slot or
//    ambiguous-state tricks, and 64-bit counters never realistically overflow.
//
//  * Each side caches the *other* side's position. The producer only re-reads
//    the consumer's atomic when its cache says the queue looks full; the
//    consumer only re-reads the producer's atomic when its cache says empty.
//    In the common case neither thread touches the other's cache line, so the
//    line holding producer_pos_ stays in the producer's L1 in Modified state
//    and isn't ping-ponged every operation.
//
//  * Producer-owned and consumer-owned state live on separate cache lines
//    (alignas + padding) so a producer write to producer_pos_ does not
//    invalidate the consumer's line (false sharing) and vice versa.
//
//  * Acquire/release on the positions publishes the slot contents: the
//    producer's release store of producer_pos_ happens-after it writes the
//    slot; the consumer's acquire load of producer_pos_ happens-before it
//    reads that slot. The slot data itself therefore needs no atomics.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "hft/compiler.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert(Capacity >= 2, "capacity must be >= 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
  static_assert(std::is_default_constructible_v<T>, "T must be default-constructible");

  static constexpr std::size_t kMask = Capacity - 1;

 public:
  using value_type = T;

  SpscQueue() = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  // ---- producer side -------------------------------------------------------
  // Returns false if the queue is full (caller decides whether to spin/drop).
  HFT_ALWAYS_INLINE bool push(const T& value) noexcept {
    return emplace_from(value);
  }
  HFT_ALWAYS_INLINE bool push(T&& value) noexcept {
    return emplace_from(std::move(value));
  }

  // ---- consumer side -------------------------------------------------------
  // Returns false if the queue is empty.
  HFT_ALWAYS_INLINE bool pop(T& out) noexcept {
    const std::size_t cons = consumer_pos_.load(std::memory_order_relaxed);
    if (cons == cached_producer_pos_) {
      // Cache says empty — confirm against the real producer position.
      cached_producer_pos_ = producer_pos_.load(std::memory_order_acquire);
      if (cons == cached_producer_pos_) return false;  // genuinely empty
    }
    out = std::move(buf_[cons & kMask]);
    consumer_pos_.store(cons + 1, std::memory_order_release);
    return true;
  }

  // ---- observation (approximate under concurrency) -------------------------
  std::size_t size_approx() const noexcept {
    const std::size_t p = producer_pos_.load(std::memory_order_acquire);
    const std::size_t c = consumer_pos_.load(std::memory_order_acquire);
    return p - c;
  }
  bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  template <typename U>
  HFT_ALWAYS_INLINE bool emplace_from(U&& value) noexcept {
    const std::size_t prod = producer_pos_.load(std::memory_order_relaxed);
    if (prod - cached_consumer_pos_ >= Capacity) {
      // Cache says full — confirm against the real consumer position.
      cached_consumer_pos_ = consumer_pos_.load(std::memory_order_acquire);
      if (prod - cached_consumer_pos_ >= Capacity) return false;  // genuinely full
    }
    buf_[prod & kMask] = std::forward<U>(value);
    producer_pos_.store(prod + 1, std::memory_order_release);
    return true;
  }

  // --- producer cache line: written by producer, position read by consumer ---
  alignas(kCacheLineSize) std::atomic<std::size_t> producer_pos_{0};
  std::size_t cached_consumer_pos_{0};  // producer-private snapshot

  // --- consumer cache line: written by consumer, position read by producer ---
  alignas(kCacheLineSize) std::atomic<std::size_t> consumer_pos_{0};
  std::size_t cached_producer_pos_{0};  // consumer-private snapshot

  // --- payload storage on its own line(s) -----------------------------------
  alignas(kCacheLineSize) std::array<T, Capacity> buf_{};
};

}  // namespace hft
