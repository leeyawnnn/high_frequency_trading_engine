// Phase 2 benchmark: SPSC ring buffer throughput.
//
// Producer and consumer run on separate threads, each pinned where the
// platform allows. We push/pop a large number of fixed-size items and report
// sustained ops/sec plus ns/op. Target: >= 100M ops/sec on a modern desktop.
#include "hft/spsc_queue.hpp"
#include "hft/tsc.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace {

void pin_to_core([[maybe_unused]] int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

// A representative payload: 32 bytes, the size of our ITCH message (Phase 3).
struct Item {
  std::uint64_t seq;
  std::uint64_t a, b, c;
};

}  // namespace

int main(int argc, char** argv) {
  hft::tsc_calibrate();

  const std::uint64_t kItems =
      (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 500'000'000ULL;

  using Queue = hft::SpscQueue<Item, 4096>;
  auto q = std::make_unique<Queue>();

  std::uint64_t checksum = 0;

  std::thread consumer([&] {
    pin_to_core(3);
    Item it{};
    std::uint64_t got = 0;
    std::uint64_t sum = 0;
    while (got < kItems) {
      if (q->pop(it)) {
        sum += it.seq;
        ++got;
      }
    }
    checksum = sum;
  });

  const std::uint64_t t0 = hft::tsc_now_serialized();

  std::thread producer([&] {
    pin_to_core(1);
    Item it{};
    for (std::uint64_t i = 0; i < kItems; ++i) {
      it.seq = i;
      while (!q->push(it)) {
      }
    }
  });

  producer.join();
  consumer.join();
  const std::uint64_t t1 = hft::tsc_now_serialized();

  const double ns = hft::tsc_to_ns(t1 - t0);
  const double ops_per_sec = static_cast<double>(kItems) / (ns / 1e9);
  const double ns_per_op = ns / static_cast<double>(kItems);

  std::printf("SPSC throughput\n");
  std::printf("  items      : %llu\n", static_cast<unsigned long long>(kItems));
  std::printf("  payload    : %zu bytes\n", sizeof(Item));
  std::printf("  elapsed    : %.1f ms\n", ns / 1e6);
  std::printf("  throughput : %.1f M ops/sec\n", ops_per_sec / 1e6);
  std::printf("  per op     : %.2f ns\n", ns_per_op);
  std::printf("  checksum   : %llu\n", static_cast<unsigned long long>(checksum));
  return 0;
}
