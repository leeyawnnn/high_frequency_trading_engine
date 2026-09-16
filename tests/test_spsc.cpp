// Correctness of the lock-free SPSC ring buffer.
//
// These cases assert the queue's protocol: FIFO order, no drops, no
// duplicates, exact empty/full boundaries, and correct index wrap. They do not
// measure throughput. Throughput is a property of the machine, not of the
// code, so it lives in benchmarks/bench_spsc.cpp where a slow result is a
// number to report rather than a build failure.
//
// Every case here is bounded by wall-clock rather than by an operation count.
// The previous version pushed a fixed 100,000,000 items, which takes under a
// second on a dev box with two idle cores and more than two minutes on a
// shared CI runner where the producer and consumer contend for one. An
// op-count bound encodes an assumption about the machine; a time bound does
// not.
#include "hft/compiler.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/wait_policy.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

// How long the concurrent case is allowed to run before it stops pushing.
// Reached only on a slow or heavily contended machine; a dev box finishes the
// item target long before this.
constexpr auto kStressBudget = std::chrono::seconds(3);

// Hard stop for the consumer, so a genuine livelock fails the test rather than
// hanging until CTest's timeout kills it with no diagnosis.
constexpr auto kConsumerDeadline = std::chrono::seconds(30);

// Item target on an unloaded machine. Large enough to wrap a 1024-slot ring
// thousands of times and to exercise the full/empty backpressure paths
// constantly; small enough to finish quickly.
constexpr std::uint64_t kTargetItems = 2'000'000;

// A run that transferred fewer than this proves nothing, so the test fails
// rather than passing vacuously on a machine that produced almost nothing.
constexpr std::uint64_t kMinimumUsefulItems = 50'000;

}  // namespace

HFT_TEST(single_threaded_roundtrip) {
  hft::SpscQueue<int, 8> q;
  int out = -1;
  CHECK(!q.pop(out));  // empty
  CHECK(q.empty_approx());
  for (int i = 0; i < 7; ++i) CHECK(q.push(i));  // fill to capacity-1
  CHECK(q.push(7));                              // 8th element fills it (capacity 8)
  CHECK(!q.push(8));                             // now full
  CHECK_EQ(q.size_approx(), 8u);
  for (int i = 0; i < 8; ++i) {
    CHECK(q.pop(out));
    CHECK_EQ(out, i);  // FIFO order preserved
  }
  CHECK(!q.pop(out));  // empty again
}

HFT_TEST(minimum_capacity_queue) {
  // Capacity 2 is the smallest the static_asserts allow, and it is where an
  // off-by-one in the full test shows up most readily: there is exactly one
  // slot between empty and full.
  hft::SpscQueue<int, 2> q;
  int out = -1;
  CHECK_EQ(q.capacity(), 2u);
  CHECK(!q.pop(out));
  CHECK(q.push(1));
  CHECK(q.push(2));
  CHECK(!q.push(3));  // full at exactly 2, not 1
  CHECK_EQ(q.size_approx(), 2u);
  CHECK(q.pop(out));
  CHECK_EQ(out, 1);
  CHECK(q.push(3));   // one slot freed, one slot usable
  CHECK(!q.push(4));  // and no more
  CHECK(q.pop(out));
  CHECK_EQ(out, 2);
  CHECK(q.pop(out));
  CHECK_EQ(out, 3);
  CHECK(!q.pop(out));
  CHECK(q.empty_approx());
}

HFT_TEST(empty_and_full_boundaries_are_exact) {
  // Fill exactly to capacity, confirm the next push fails, drain exactly to
  // empty, confirm the next pop fails. Off-by-one in either direction shows up
  // here as either a lost slot or an overwrite.
  constexpr std::size_t kCap = 16;
  hft::SpscQueue<std::uint32_t, kCap> q;
  std::uint32_t out = 0;

  for (std::uint32_t i = 0; i < kCap; ++i) {
    CHECK(q.push(i));
    CHECK_EQ(q.size_approx(), static_cast<std::size_t>(i) + 1);
  }
  CHECK(!q.push(999));  // exactly full
  CHECK_EQ(q.size_approx(), kCap);

  for (std::uint32_t i = 0; i < kCap; ++i) {
    CHECK(q.pop(out));
    CHECK_EQ(out, i);
    CHECK_EQ(q.size_approx(), kCap - i - 1);
  }
  CHECK(!q.pop(out));  // exactly empty
  CHECK(q.empty_approx());
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

HFT_TEST(partial_drain_keeps_order_across_wrap) {
  // Never let the queue go empty, so the read and write indices wrap at
  // different times and the mask arithmetic is exercised while both sides hold
  // live data -- the case a fill-then-drain loop never reaches.
  hft::SpscQueue<std::uint64_t, 8> q;
  std::uint64_t next_push = 0;
  std::uint64_t next_expect = 0;
  std::uint64_t out = 0;

  for (int i = 0; i < 4; ++i) CHECK(q.push(next_push++));
  for (int round = 0; round < 5000; ++round) {
    CHECK(q.push(next_push++));
    CHECK(q.push(next_push++));
    CHECK(q.pop(out));
    CHECK_EQ(out, next_expect++);
    CHECK(q.pop(out));
    CHECK_EQ(out, next_expect++);
  }
  while (q.pop(out)) {
    CHECK_EQ(out, next_expect++);
  }
  CHECK_EQ(next_expect, next_push);
}

HFT_TEST(two_thread_no_drops_no_dups) {
  // Producer pushes a strictly increasing sequence; consumer verifies it sees
  // exactly that sequence, in order, with none missing and none repeated.
  // Capacity is small relative to the item count so backpressure (full/empty
  // waiting) is exercised constantly.
  using Queue = hft::SpscQueue<std::uint64_t, 1024>;
  auto q = std::make_unique<Queue>();

  std::atomic<bool> producer_done{false};
  std::atomic<bool> consumer_out_of_order{false};
  std::atomic<bool> consumer_timed_out{false};
  std::atomic<std::uint64_t> produced{0};
  std::atomic<std::uint64_t> consumed{0};

  const auto push_deadline = Clock::now() + kStressBudget;
  const auto consumer_deadline = Clock::now() + kConsumerDeadline;

  std::thread consumer([&] {
    // Tests never pin, so the unpinned policy is the honest one: spin briefly,
    // then hand the core back. Without this the two threads starve each other
    // on any machine that cannot give them a core each.
    hft::WaitStrategy wait(false);
    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    for (;;) {
      if (q->pop(value)) {
        if (value != expected) {  // out of order => drop or dup
          consumer_out_of_order.store(true, std::memory_order_relaxed);
          break;
        }
        ++expected;
        wait.reset();
        continue;
      }
      // Empty. Stop only once the producer is finished and nothing is left.
      if (producer_done.load(std::memory_order_acquire) && q->empty_approx()) break;
      if (Clock::now() >= consumer_deadline) {
        consumer_timed_out.store(true, std::memory_order_relaxed);
        break;
      }
      wait.idle();
    }
    consumed.store(expected, std::memory_order_relaxed);
  });

  std::thread producer([&] {
    hft::WaitStrategy wait(false);
    std::uint64_t i = 0;
    for (; i < kTargetItems; ++i) {
      while (!q->push(i)) {
        if (Clock::now() >= push_deadline) {
          // Out of time, not out of correctness. Stop here and let the
          // consumer drain what was actually sent.
          produced.store(i, std::memory_order_relaxed);
          producer_done.store(true, std::memory_order_release);
          return;
        }
        wait.idle();
      }
      wait.reset();
    }
    produced.store(i, std::memory_order_relaxed);
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  CHECK(!consumer_out_of_order.load());
  CHECK(!consumer_timed_out.load());
  CHECK_EQ(consumed.load(), produced.load());  // every item observed exactly once
  CHECK(q->empty_approx());                    // nothing stranded in the buffer
  // Guard against a vacuous pass: a run that moved almost nothing has not
  // exercised the backpressure paths this case exists to cover.
  CHECK(produced.load() >= kMinimumUsefulItems);
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
