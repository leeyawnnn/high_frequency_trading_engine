// exchange_sim — standalone simulated exchange process (thin CLI wrapper over
// hft::SimExchange). Generates an ITCH-style feed and answers orders with fills.
//
// Usage:
//   exchange_sim [--feed-host H] [--feed-port P] [--order-port P]
//                [--fill-host H] [--fill-port P] [--symbol S]
//                [--rate MSGS_PER_SEC] [--batch N] [--duration-ms MS] [--cpu CORE]
#include "hft/itch_message.hpp"
#include "hft/net.hpp"
#include "hft/sim_exchange.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }

void pin_cpu([[maybe_unused]] int core) {
#if defined(__linux__)
  if (core < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

const char* arg_str(int argc, char** argv, const char* key, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}
long arg_long(int argc, char** argv, const char* key, long def) {
  const char* s = arg_str(argc, argv, key, nullptr);
  return s ? std::strtol(s, nullptr, 10) : def;
}
}  // namespace

int main(int argc, char** argv) {
  const char* feed_host = arg_str(argc, argv, "--feed-host", "127.0.0.1");
  const auto feed_port = static_cast<std::uint16_t>(arg_long(argc, argv, "--feed-port", 31337));
  const auto order_port = static_cast<std::uint16_t>(arg_long(argc, argv, "--order-port", 31338));
  const char* fill_host = arg_str(argc, argv, "--fill-host", "127.0.0.1");
  const auto fill_port = static_cast<std::uint16_t>(arg_long(argc, argv, "--fill-port", 31339));
  const char* symbol = arg_str(argc, argv, "--symbol", "TST");
  const long rate = arg_long(argc, argv, "--rate", 1'000'000);
  const long batch = arg_long(argc, argv, "--batch", 1);
  const long duration_ms = arg_long(argc, argv, "--duration-ms", 0);
  const int cpu = static_cast<int>(arg_long(argc, argv, "--cpu", -1));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  pin_cpu(cpu);
  hft::tsc_calibrate();

  hft::SimExchange::Config cfg{
      hft::Endpoint::v4(feed_host, feed_port),
      hft::Endpoint::v4(fill_host, fill_port),
      order_port,
      hft::pack_symbol(symbol),
      rate,
      batch,
      0x5EED,
      hft::price_from_double(100.0),
      hft::kPriceScale / 100,
  };
  hft::SimExchange exch(cfg);

  const std::uint64_t start = hft::tsc_now();
  std::thread stopper;
  if (duration_ms > 0) {
    stopper = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
      g_running.store(false);
    });
  }

  exch.run(g_running);
  if (stopper.joinable()) stopper.join();

  const double elapsed_ns = hft::tsc_to_ns(hft::tsc_now() - start);
  std::fprintf(stderr,
               "exchange_sim: feed=%llu msgs (%.2f M/s), orders=%llu, fills=%llu, %.1f ms\n",
               static_cast<unsigned long long>(exch.feed_count()),
               static_cast<double>(exch.feed_count()) / (elapsed_ns / 1e9) / 1e6,
               static_cast<unsigned long long>(exch.order_count()),
               static_cast<unsigned long long>(exch.fill_count()), elapsed_ns / 1e6);
  return 0;
}
