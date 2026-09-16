// Phase 10: full-engine integration. Runs the wired Engine and a SimExchange
// in-process over loopback and checks that the whole pipeline carries data end
// to end — orders flow, fills come back, and the e2e latency histogram fills.
#include "hft/engine.hpp"
#include "hft/sim_exchange.hpp"
#include "hft/tsc.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// Sanitizer instrumentation (esp. TSan, ~20x slower) inflates wall-clock
// latency by orders of magnitude, so the latency-magnitude sanity check below
// is only meaningful in an uninstrumented build. TSan/ASan still validate
// correctness and data-race freedom of the full pipeline.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define HFT_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define HFT_SANITIZED 1
#endif

HFT_TEST(end_to_end_pipeline_carries_data) {
  hft::tsc_calibrate();

  // Engine on ephemeral feed/fill ports.
  hft::EngineConfig ecfg;
  ecfg.feed_port = 0;
  ecfg.fill_port = 0;
  ecfg.feed_bind = "127.0.0.1";
  ecfg.fill_bind = "127.0.0.1";
  ecfg.order_dst = hft::Endpoint::v4("127.0.0.1", 1);  // set after exch exists
  auto engine = std::make_unique<hft::Engine<>>(ecfg);

  // Exchange on an ephemeral order port, moderate rate.
  hft::SimExchange::Config xcfg{
      hft::Endpoint::v4("127.0.0.1", 1),
      hft::Endpoint::v4("127.0.0.1", 1),
      0,
      hft::pack_symbol("TST"),
      /*rate*/ 300'000,
      /*batch*/ 8,
      0x5EED,
      hft::price_from_double(100.0),
      hft::kPriceScale / 100,
  };
  hft::SimExchange exch(xcfg);

  // Cross-wire ephemeral ports.
  engine->set_order_destination(hft::Endpoint::v4("127.0.0.1", exch.order_local_port()));
  exch.set_feed_dst(hft::Endpoint::v4("127.0.0.1", engine->feed_local_port()));
  exch.set_fill_dst(hft::Endpoint::v4("127.0.0.1", engine->fill_local_port()));

  std::atomic<bool> exch_running{true};
  engine->start();
  std::thread exch_thread([&] { exch.run(exch_running); });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  exch_running.store(false);
  exch_thread.join();
  engine->stop();

  // Data flowed through every stage.
  CHECK(exch.feed_count() > 0);
  CHECK(engine->strategy().processed() > 0);
  CHECK(engine->feed_latency().count() > 0);
  CHECK(engine->strategy_latency().count() > 0);

  // The strategy's imbalance signal fired and orders round-tripped to fills.
  CHECK(engine->gateway().sent() > 0);
  CHECK(engine->gateway().fills_matched() > 0);
  CHECK(engine->e2e_latency().count() > 0);
  CHECK(engine->round_trip_latency().count() > 0);

  // No spurious risk rejections or mismatched fills under normal limits.
  CHECK_EQ(engine->gateway().risk_rejected(), 0u);
  CHECK_EQ(engine->gateway().fills_unmatched(), 0u);

  // Position stays bounded by the single-unit, in-flight-gated logic.
  CHECK(engine->strategy().strategy().position() <= 100);
  CHECK(engine->strategy().strategy().position() >= -100);

  // The latency histogram is internally consistent.
  //
  // This used to assert that the end-to-end p50 was under a millisecond, with
  // the threshold compiled out under sanitizers because their slowdown broke
  // it. That exemption was the tell: a wall-clock bound is a property of the
  // machine, and this machine is whatever CI happened to schedule the job on.
  // It failed reliably on Debug legs and on macOS runners, and it can be
  // reproduced on any box by running this binary while the cores are busy --
  // the median rises past a millisecond and a correctness test reports a
  // defect that is not there.
  //
  // What can be asserted without knowing the machine is that the measurement
  // is coherent: samples exist, the percentiles are ordered, and nothing
  // exceeds the histogram's trackable range. The latency budget itself belongs
  // to benchmarks/ and to the published report, where a slow number is a
  // number to explain rather than a build failure.
  // Note these are bucket upper bounds, not observed values: percentile() may
  // legitimately exceed max(), so the invariant is monotonicity across
  // percentiles rather than any relation to max.
  const auto& e2e = engine->e2e_latency();
  CHECK(e2e.count() > 0);
  CHECK(e2e.percentile(50.0) <= e2e.percentile(99.0));
  CHECK(e2e.percentile(99.0) <= e2e.percentile(100.0));
  CHECK(e2e.min() <= e2e.max());
  CHECK(e2e.max() <= e2e.percentile(100.0));
}

HFT_TEST_MAIN()
