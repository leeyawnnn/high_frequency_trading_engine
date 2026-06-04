// latency_report — self-contained end-to-end latency harness.
//
// Runs the full Engine AND a SimExchange in one process over loopback UDP, so a
// single command produces the four-stage latency report (feed / strategy /
// gateway / end-to-end). Ephemeral ports are wired up after both ends exist, so
// there are no fixed-port collisions.
//
// Usage: latency_report [--duration-ms MS] [--rate MSGS_PER_SEC] [--batch N]
//                       [--feed-core C] [--strat-core C] [--gw-core C] [--exch-core C]
#include "hft/engine.hpp"
#include "hft/latency_report.hpp"
#include "hft/sim_exchange.hpp"
#include "hft/affinity.hpp"
#include "hft/tsc.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
long arg_long(int argc, char** argv, const char* key, long def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
  return def;
}
const char* arg_str(int argc, char** argv, const char* key, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}
}  // namespace

int main(int argc, char** argv) {
  const long duration_ms = arg_long(argc, argv, "--duration-ms", 3000);
  const long rate = arg_long(argc, argv, "--rate", 500'000);
  const long batch = arg_long(argc, argv, "--batch", 8);
  const int feed_core = static_cast<int>(arg_long(argc, argv, "--feed-core", -1));
  const int strat_core = static_cast<int>(arg_long(argc, argv, "--strat-core", -1));
  const int gw_core = static_cast<int>(arg_long(argc, argv, "--gw-core", -1));
  const int exch_core = static_cast<int>(arg_long(argc, argv, "--exch-core", -1));
  const char* csv_dir = arg_str(argc, argv, "--csv-dir", nullptr);

  hft::tsc_calibrate();

  // 1. Build the engine on ephemeral feed/fill ports.
  hft::EngineConfig ecfg;
  ecfg.feed_core = feed_core;
  ecfg.strategy_core = strat_core;
  ecfg.gateway_core = gw_core;
  ecfg.feed_port = 0;
  ecfg.fill_port = 0;
  ecfg.feed_bind = "127.0.0.1";
  ecfg.fill_bind = "127.0.0.1";
  ecfg.order_dst = hft::Endpoint::v4("127.0.0.1", 1);  // placeholder, set below
  auto engine = std::make_unique<hft::Engine<>>(ecfg);

  // 2. Build the exchange on an ephemeral order port.
  hft::SimExchange::Config xcfg{
      hft::Endpoint::v4("127.0.0.1", 1),  // feed_dst placeholder
      hft::Endpoint::v4("127.0.0.1", 1),  // fill_dst placeholder
      0,                                  // order bind ephemeral
      hft::pack_symbol("TST"),
      rate, batch, 0x5EED,
      hft::price_from_double(100.0), hft::kPriceScale / 100,
  };
  hft::SimExchange exch(xcfg);

  // 3. Cross-wire the ephemeral ports.
  engine->set_order_destination(hft::Endpoint::v4("127.0.0.1", exch.order_local_port()));
  exch.set_feed_dst(hft::Endpoint::v4("127.0.0.1", engine->feed_local_port()));
  exch.set_fill_dst(hft::Endpoint::v4("127.0.0.1", engine->fill_local_port()));

  std::printf("latency_report: rate=%ld msg/s batch=%ld duration=%ldms "
              "(cores feed=%d strat=%d gw=%d exch=%d)\n",
              rate, batch, duration_ms, feed_core, strat_core, gw_core, exch_core);

  // 4. Run.
  std::atomic<bool> exch_running{true};
  engine->start();
  std::thread exch_thread([&] {
    hft::pin_current_thread(exch_core);
    exch.run(exch_running);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

  exch_running.store(false);
  exch_thread.join();
  engine->stop();

  // 5. Report.
  std::printf("\nthroughput: exchange sent %llu feed msgs, engine processed %llu, "
              "orders sent %llu, fills matched %llu\n",
              static_cast<unsigned long long>(exch.feed_count()),
              static_cast<unsigned long long>(engine->strategy().processed()),
              static_cast<unsigned long long>(engine->gateway().sent()),
              static_cast<unsigned long long>(engine->gateway().fills_matched()));

  hft::print_engine_report(engine->feed_latency(), engine->strategy_latency(),
                           engine->gateway_latency(), engine->e2e_latency(),
                           engine->round_trip_latency());

  if (csv_dir) {
    const std::string d = csv_dir;
    hft::dump_histogram_csv(d + "/feed.csv", engine->feed_latency());
    hft::dump_histogram_csv(d + "/strategy.csv", engine->strategy_latency());
    hft::dump_histogram_csv(d + "/gateway.csv", engine->gateway_latency());
    hft::dump_histogram_csv(d + "/e2e.csv", engine->e2e_latency());
    hft::dump_histogram_csv(d + "/round_trip.csv", engine->round_trip_latency());
    if (std::FILE* f = std::fopen((d + "/summary.csv").c_str(), "w")) {
      std::fprintf(f, "stage,n,p50,p90,p99,p999,max,mean\n");
      hft::write_summary_row(f, "feed", engine->feed_latency());
      hft::write_summary_row(f, "strategy", engine->strategy_latency());
      hft::write_summary_row(f, "gateway", engine->gateway_latency());
      hft::write_summary_row(f, "end_to_end", engine->e2e_latency());
      hft::write_summary_row(f, "round_trip", engine->round_trip_latency());
      std::fclose(f);
    }
    std::printf("\nCSV written to %s/\n", csv_dir);
  }
  return 0;
}
