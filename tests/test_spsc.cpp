// Phase 2: lock-free SPSC ring buffer.
#include "test_harness.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/compiler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

// Sanitizer builds run the same logic but at a reduced item count (TSan in
// particular is ~20x slower) — correctness of the protocol, not throughput, is
// what those builds verify.
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define HFT_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define HFT_SANITIZED 1
#endif

namespace {
#if defined(HFT_SANITIZED)
constexpr std::uint64_t kStressItems = 3'000'000;
#else
constexpr std::uint64_t kStressItems = 100'000'000;
#endif
}  // namespace

HFT_TEST(single_threaded_roundtrip) {
  hft::SpscQueue<int, 8> q;
  int out = -1;
  CHECK(!q.pop(out));        // empty
  CHECK(q.empty_approx());
  for (int i = 0; i < 7; ++i) CHECK(q.push(i));  // fill to capacity-1
  CHECK(q.push(7));          // 8th element fills it (capacity 8)
  CHECK(!q.push(8));         // now full
  CHECK_EQ(q.size_approx(), 8u);
  for (int i = 0; i < 8; ++i) {
    CHECK(q.pop(out));
    CHECK_EQ(out, i);        // FIFO order preserved
  }
  CHECK(!q.pop(out));        // empty again
}

HFT_TEST(wraparound_many_cycles) {
  // Repeatedly fill/drain a small queue to exercise index wrap (& mask).
  hft::SpscQueue<std::uint32_t, 4> q;
  std::uint32_t out = 0;
  std::uint32_t expected = 0;
  for (int cycle = 0; cycle < 1000; ++cycle) {
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(q.push(expected + i));
    for (std::uint32_t i = 0; i < 4; ++i) {
      CHECK(q.pop(out));
      CHECK_EQ(out, expected + i);
    }
    expected += 4;
  }
}

HFT_TEST(two_thread_no_drops_no_dups) {
  // Producer pushes a strictly increasing sequence; consumer verifies it sees
  // exactly that sequence, in order, with none missing and none repeated.
  // Capacity is small relative to the item count so backpressure (full/empty
  // spinning) is exercised constantly.
  using Queue = hft::SpscQueue<std::uint64_t, 1024>;
  auto q = std::make_unique<Queue>();

  std::atomic<bool> consumer_failed{false};
  std::atomic<std::uint64_t> consumed{0};

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    while (expected < kStressItems) {
      if (q->pop(value)) {
        if (value != expected) {        // out of order => drop or dup
          consumer_failed.store(true, std::memory_order_relaxed);
          break;
        }
        ++expected;
      } else {
        // empty: busy-wait (no sleep — this is the realistic hot-loop shape)
      }
    }
    consumed.store(expected, std::memory_order_relaxed);
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kStressItems; ++i) {
      while (!q->push(i)) {
        // full: spin until the consumer drains a slot
      }
    }
  });

  producer.join();
  consumer.join();

  CHECK(!consumer_failed.load());
  CHECK_EQ(consumed.load(), kStressItems);  // every item observed exactly once
  CHECK(q->empty_approx());                 // nothing stranded in the buffer
}

HFT_TEST(layout_avoids_false_sharing) {
  // The whole queue must be cache-line aligned, and the buffer must be large
  // (proxy check that producer/consumer state and payload don't co-reside).
  using Queue = hft::SpscQueue<std::uint64_t, 64>;
  CHECK_EQ(alignof(Queue) % hft::kCacheLineSize, 0u);
  // Producer state + consumer state + buffer each on their own line(s) means
  // the object spans at least three cache lines.
  CHECK(sizeof(Queue) >= 3 * hft::kCacheLineSize);
}

HFT_TEST_MAIN()
