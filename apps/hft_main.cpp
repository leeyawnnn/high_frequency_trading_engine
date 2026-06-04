// hft_main — the wired engine as a standalone process.
//
// Runs the three pinned pipeline threads (feed handler, strategy, gateway) and
// talks to a SEPARATELY-running exchange_sim over loopback UDP. Configuration
// (core pinning, ports, strategy/risk parameters) comes from a JSON file.
//
// Typical session (two terminals):
//   ./exchange_sim --feed-port 31337 --order-port 31338 --fill-port 31339 --batch 8
//   ./hft_main --config config/engine.example.json --duration-ms 5000
//
// On Linux pin to isolated cores (isolcpus=) for stable numbers. On macOS the
// pinning is a no-op (see README); the engine still runs for development.
#include "hft/config.hpp"
#include "hft/engine.hpp"
#include "hft/latency_report.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

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
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  hft::tsc_calibrate();

  const char* config_path = arg_str(argc, argv, "--config", nullptr);
  const long duration_ms = arg_long(argc, argv, "--duration-ms", 0);  // 0 = until SIGINT

  hft::EngineConfig cfg;  // defaults match exchange_sim defaults
  if (config_path) {
    try {
      const hft::Config c = hft::Config::load_file(config_path);
      cfg.feed_core = static_cast<int>(c.get_int("cores.feed_handler", -1));
      cfg.strategy_core = static_cast<int>(c.get_int("cores.strategy", -1));
      cfg.gateway_core = static_cast<int>(c.get_int("cores.order_gateway", -1));
      cfg.feed_port = static_cast<std::uint16_t>(c.get_int("network.feed_port", 31337));
      const std::string order_host = c.get_string("network.order_addr", "127.0.0.1");
      const auto order_port = static_cast<std::uint16_t>(c.get_int("network.order_port", 31338));
      cfg.order_dst = hft::Endpoint::v4(order_host.c_str(), order_port);
      cfg.fill_port = static_cast<std::uint16_t>(c.get_int("network.fill_port", 31339));
      cfg.threshold = c.get_double("strategy.imbalance_threshold", 0.30);
      cfg.order_size = static_cast<std::uint32_t>(c.get_int("strategy.order_size", 100));
      cfg.risk.max_position = c.get_int("risk.max_position", 1000);
      cfg.risk.max_order_size = static_cast<std::uint32_t>(c.get_int("risk.max_order_size", 500));
      cfg.risk.max_orders_per_sec = c.get_double("risk.rate_limit_per_sec", 0.0);
      cfg.risk.burst = static_cast<std::uint32_t>(c.get_int("risk.burst", 100));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "config error: %s\n", e.what());
      return 1;
    }
  }

  std::printf("hft_main: feed_port=%u order_dst_port=%u fill_port=%u "
              "cores(feed=%d strat=%d gw=%d) threshold=%.2f\n",
              cfg.feed_port, 0u /*shown via config*/, cfg.fill_port,
              cfg.feed_core, cfg.strategy_core, cfg.gateway_core, cfg.threshold);
  std::printf("hft_main: ensure exchange_sim is running on matching ports.\n");

  auto engine = std::make_unique<hft::Engine<>>(cfg);
  engine->start();

  if (duration_ms > 0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } else {
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  engine->stop();

  std::printf("\norders sent=%llu  fills matched=%llu  risk rejected=%llu\n",
              static_cast<unsigned long long>(engine->gateway().sent()),
              static_cast<unsigned long long>(engine->gateway().fills_matched()),
              static_cast<unsigned long long>(engine->gateway().risk_rejected()));
  hft::print_engine_report(engine->feed_latency(), engine->strategy_latency(),
                           engine->gateway_latency(), engine->e2e_latency(),
                           engine->round_trip_latency());
  return 0;
}
