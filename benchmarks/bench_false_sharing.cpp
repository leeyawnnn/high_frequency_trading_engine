// Padded vs unpadded SPSC queue: does cache-line separation pay here?
//
// This program is the producer for reports/data/false_sharing.csv. Until it
// was written there was none, and the README's figure argued a substantive
// claim -- that the unpadded layout can win on a clustered-core machine --
// from a file no committed command could regenerate.
//
// The two queues are identical except for where the producer and consumer
// cursors live. The padded one is hft::SpscQueue, whose cursors sit on their
// own cache lines. The unpadded one below declares them adjacent, so both fall
// in the same line and every producer store invalidates the consumer's copy of
// it. That invalidation is the whole experiment.
//
// The result is machine-dependent by nature, which is the point: the CSV
// carries the core count and the cache-line size it was measured with, so a
// number from one topology is not read as a claim about another.
#include "hft/affinity.hpp"
#include "hft/compiler.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/tsc.hpp"
#include "hft/wait_policy.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kCapacity = 4096;
constexpr std::uint64_t kItems = 20'000'000;
constexpr int kReps = 5;

// Deliberately unpadded: the two cursors share a cache line.
//
// Everything else here is a line-for-line copy of hft::SpscQueue, including
// the cached opposite-cursor snapshots. That matters: a first version of this
// class omitted the caching, so it was slower for two reasons at once and the
// experiment could not attribute the difference to padding. The ONLY
// difference from the real queue is the missing alignas.
template <typename T, std::size_t Capacity>
class UnpaddedSpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

 public:
  bool push(const T& v) noexcept {
    const std::size_t p = producer_pos_.load(std::memory_order_relaxed);
    if (p - cached_consumer_pos_ >= Capacity) {
      cached_consumer_pos_ = consumer_pos_.load(std::memory_order_acquire);
      if (p - cached_consumer_pos_ >= Capacity) return false;
    }
    buf_[p & kMask] = v;
    producer_pos_.store(p + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) noexcept {
    const std::size_t c = consumer_pos_.load(std::memory_order_relaxed);
    if (c == cached_producer_pos_) {
      cached_producer_pos_ = producer_pos_.load(std::memory_order_acquire);
      if (c == cached_producer_pos_) return false;
    }
    out = buf_[c & kMask];
    consumer_pos_.store(c + 1, std::memory_order_release);
    return true;
  }

 private:
  // No alignas, and adjacent: these all land on one or two shared lines.
  std::atomic<std::size_t> producer_pos_{0};
  std::size_t cached_consumer_pos_{0};
  std::atomic<std::size_t> consumer_pos_{0};
  std::size_t cached_producer_pos_{0};
  std::array<T, Capacity> buf_{};
};

struct Item {
  std::uint64_t seq;
  std::uint64_t a, b, c;
};

// Parse a core index, refusing anything that is not an integer. -1 means
// "do not pin" and is the documented default.
int parse_core(const char* s) {
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < -1 || v > 4096) {
    std::fprintf(stderr, "invalid core index '%s'; expected -1 or a small integer\n", s);
    std::exit(2);
  }
  return static_cast<int>(v);
}

// Returns millions of ops/sec for one run of `Queue`.
template <typename Queue>
double run_once(int prod_core, int cons_core) {
  auto q = std::make_unique<Queue>();
  std::uint64_t checksum = 0;

  std::thread consumer([&] {
    (void)hft::pin_and_name_thread(cons_core, "fs-cons");
    // Spin, regardless of whether the pin succeeded. This benchmark measures
    // the cost of a cache-line invalidation; yielding to the scheduler would
    // measure the scheduler instead and swamp the effect entirely. Measured
    // with the yield policy on an idle 10-core box, both variants collapsed to
    // 1.2 M ops/s and the difference vanished. That requires a free core per
    // thread, which is a benchmark precondition, not a deployment assumption.
    hft::WaitStrategy wait(true);
    Item it{};
    std::uint64_t got = 0;
    std::uint64_t sum = 0;
    while (got < kItems) {
      if (q->pop(it)) {
        hft::DoNotOptimize(it);
        sum += it.seq;
        ++got;
        wait.reset();
      } else {
        wait.idle();
      }
    }
    checksum = sum;
  });

  const std::uint64_t t0 = hft::tsc_now_serialized();
  std::thread producer([&] {
    (void)hft::pin_and_name_thread(prod_core, "fs-prod");
    hft::WaitStrategy wait(true);  // see the consumer's note

    Item it{};
    for (std::uint64_t i = 0; i < kItems; ++i) {
      it.seq = i;
      hft::DoNotOptimize(it);
      while (!q->push(it)) wait.idle();
      wait.reset();
    }
  });

  producer.join();
  consumer.join();
  const std::uint64_t t1 = hft::tsc_now_serialized();
  hft::DoNotOptimize(checksum);

  const double ns = hft::tsc_to_ns(t1 - t0);
  return static_cast<double>(kItems) / (ns / 1e9) / 1e6;
}

template <typename Queue>
double median_mops(int prod_core, int cons_core) {
  std::vector<double> runs;
  runs.reserve(static_cast<std::size_t>(kReps));
  for (int i = 0; i < kReps; ++i) runs.push_back(run_once<Queue>(prod_core, cons_core));
  std::sort(runs.begin(), runs.end());
  return runs[runs.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  hft::tsc_calibrate();
  // strtol, not atoi: atoi cannot report a bad argument, so a typo would
  // silently become core 0 and quietly change what is being measured.
  const int prod_core = (argc > 1) ? parse_core(argv[1]) : -1;
  const int cons_core = (argc > 2) ? parse_core(argv[2]) : -1;

  std::printf("==================== measurement conditions ====================\n");
  hft::print_clock_report(stdout);
  std::printf("cache-line size   : %zu bytes (compile-time constant)\n", hft::kCacheLineSize);
  std::printf("hardware threads  : %u\n", std::thread::hardware_concurrency());
  std::printf("requested cores   : producer=%d consumer=%d (-1 = unpinned)\n", prod_core,
              cons_core);
  std::printf("wait policy       : spin (a yield policy would measure the scheduler)\n");
  std::printf("repetitions       : %d (median reported)\n", kReps);
  std::printf("items per run     : %llu\n", static_cast<unsigned long long>(kItems));
  std::printf("================================================================\n\n");

  const double padded = median_mops<hft::SpscQueue<Item, kCapacity>>(prod_core, cons_core);
  const double unpadded = median_mops<UnpaddedSpscQueue<Item, kCapacity>>(prod_core, cons_core);

  std::printf("padded (cache-line separated) : %8.1f M ops/s\n", padded);
  std::printf("unpadded (false sharing)      : %8.1f M ops/s\n", unpadded);
  std::printf("ratio padded/unpadded         : %8.2fx\n", padded / unpadded);

  if (std::FILE* f = std::fopen("reports/data/false_sharing.csv", "w")) {
    std::fprintf(f, "variant,mops,reps,items_per_run,cache_line_bytes,hardware_threads,pinned\n");
    const char* pinned = (prod_core >= 0 && cons_core >= 0) ? "requested" : "no";
    std::fprintf(f, "padded (cache-line separated),%.1f,%d,%llu,%zu,%u,%s\n", padded, kReps,
                 static_cast<unsigned long long>(kItems), hft::kCacheLineSize,
                 std::thread::hardware_concurrency(), pinned);
    std::fprintf(f, "unpadded (false sharing),%.1f,%d,%llu,%zu,%u,%s\n", unpadded, kReps,
                 static_cast<unsigned long long>(kItems), hft::kCacheLineSize,
                 std::thread::hardware_concurrency(), pinned);
    std::fclose(f);
    std::printf("\nwrote reports/data/false_sharing.csv\n");
  } else {
    std::fprintf(stderr, "could not write reports/data/false_sharing.csv (run from repo root)\n");
    return 1;
  }
  return 0;
}
