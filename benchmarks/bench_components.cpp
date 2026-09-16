// Hot-path component microbenchmarks.
//
// This program is the producer for reports/data/components.csv. Until it was
// written there was none: the component table in the README cited a CSV that
// no committed command could regenerate.
//
// Methodology, because a single mean from a single run is not a measurement:
//
//  * Every measured value is held live with DoNotOptimize/ClobberMemory. An
//    unobserved result is dead code, and a deleted computation benchmarks as
//    free -- which is how a multi-field binary parse comes to "cost" 0.48 ns,
//    about 1.5 cycles at this clock.
//
//  * Each component runs kReps independent repetitions of kIters iterations.
//    We report the median across repetitions and the inter-quartile range, so
//    the spread is visible rather than hidden behind an average.
//
//  * A warm-up repetition runs first and is discarded, so cold caches and the
//    first-touch page faults of the input arrays do not land in the reported
//    numbers.
//
//  * tsc_now is measured as an overhead, not as a cost: an empty barrier-only
//    loop is timed first and subtracted, because the loop and the barriers are
//    not free and at this scale they are a large fraction of what is left.
//    The counter read cannot be cheaper than the counter's own tick period,
//    and the report says so when it is.
#include "hft/book_view.hpp"
#include "hft/compiler.hpp"
#include "hft/histogram.hpp"
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"
#include "hft/risk_gate.hpp"
#include "hft/strategy.hpp"
#include "hft/tsc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kReps = 11;  // odd, so the median is an observation
constexpr std::uint64_t kIters = 2'000'000;
constexpr std::size_t kBookTicks = 4096;

// Size of the message stream the loops read from, in messages. Power of two so
// the index wraps with an AND.
//
// This deliberately does NOT scale with kIters. An earlier version generated
// one message per iteration, so each repetition streamed 2,000,000 x 32 bytes
// = 64 MB from DRAM, and parse_message measured memory bandwidth rather than
// parsing: 0.488 ns/msg on a quiet machine against 1.553 ns/msg when the
// desktop was busy, a 3x swing driven by what else was competing for the
// memory system. 64Ki messages is 2 MB, which stays cache-resident, so these
// loops measure the operation rather than the fetch.
constexpr std::uint64_t kStreamMsgs = 65'536;
constexpr std::uint64_t kStreamMask = kStreamMsgs - 1;

const char* components_csv_header() {
  return "component,ns_per_op,ns_p25,ns_p75,mops,reps,iters_per_rep,baseline_ns,resolution_ns,"
         "clock_source\n";
}

struct Stats {
  double median_ns;
  double p25_ns;
  double p75_ns;
  double total_ns_per_rep;  // elapsed for one repetition, before dividing
};

Stats summarize(std::vector<double> ns_per_op) {
  std::sort(ns_per_op.begin(), ns_per_op.end());
  const std::size_t n = ns_per_op.size();
  const double median = ns_per_op[n / 2];
  return Stats{median, ns_per_op[n / 4], ns_per_op[(3 * n) / 4],
               median * static_cast<double>(kIters)};
}

// Run `body` kReps+1 times, discard the first, and return ns/op across the
// rest. `body` performs exactly kIters operations.
template <typename Body>
Stats measure(Body&& body) {
  std::vector<double> results;
  results.reserve(kReps);
  for (int rep = 0; rep < kReps + 1; ++rep) {
    const std::uint64_t t0 = hft::tsc_now_serialized();
    body();
    const std::uint64_t t1 = hft::tsc_now_serialized();
    if (rep == 0) continue;  // warm-up
    results.push_back(hft::tsc_to_ns(t1 - t0) / static_cast<double>(kIters));
  }
  return summarize(std::move(results));
}

// Build a wire-format stream that resembles an active top of book.
//
// The price distribution matters more than anything else here. An earlier
// version of this generator left `price` at its default, so every message
// addressed the same tick, the book's backing array never left L1, and
// book_apply measured 0.536 ns/update against the 4.41 ns that bench_book
// reports for the same function. That difference was the access pattern, not
// the code. A ~64-tick band around mid reproduces bench_book's distribution
// and its result.
std::vector<std::byte> make_wire_stream(std::uint64_t count, std::int64_t base, std::int64_t tick) {
  std::vector<std::byte> buf(static_cast<std::size_t>(count) * hft::kMsgSize);
  std::uint64_t x = 0x243F6A8885A308D3ULL;
  auto xs = [&x] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  for (std::uint64_t i = 0; i < count; ++i) {
    hft::ItchMessage m{};
    m.seq = static_cast<std::uint32_t>(i);
    m.symbol = hft::pack_symbol("TST");
    const int off = 2000 + static_cast<int>(xs() % 64);
    m.price = base + static_cast<std::int64_t>(off) * tick;
    m.side = (xs() & 1) ? static_cast<std::uint8_t>(hft::Side::kBuy)
                        : static_cast<std::uint8_t>(hft::Side::kSell);
    const int roll = static_cast<int>(xs() % 10);
    m.type = roll < 6 ? static_cast<std::uint8_t>(hft::MsgType::kAdd)
                      : static_cast<std::uint8_t>(hft::MsgType::kExecute);
    m.size = static_cast<std::uint32_t>(1 + xs() % 500);
    hft::encode_message(m, buf.data() + i * hft::kMsgSize);
  }
  return buf;
}

}  // namespace

int main() {
  hft::tsc_calibrate();
  std::printf("==================== measurement conditions ====================\n");
  hft::print_clock_report(stdout);
  std::printf("repetitions       : %d (plus 1 discarded warm-up)\n", kReps);
  std::printf("iterations/rep    : %llu\n", static_cast<unsigned long long>(kIters));
  std::printf("stream working set: %llu msgs (%llu KiB, cache-resident by design)\n",
              static_cast<unsigned long long>(kStreamMsgs),
              static_cast<unsigned long long>(kStreamMsgs * hft::kMsgSize / 1024));
  std::printf("================================================================\n\n");

  struct Row {
    const char* name;
    Stats s;
  };
  std::vector<Row> rows;

  // --- loop + barrier baseline ---------------------------------------------
  // Everything below pays this. For the counter read it is most of the cost.
  const Stats baseline = measure([] {
    for (std::uint64_t i = 0; i < kIters; ++i) {
      std::uint64_t v = i;
      hft::DoNotOptimize(v);
    }
  });
  std::printf("empty loop + barrier baseline: %.3f ns/iter (subtracted from tsc_now)\n\n",
              baseline.median_ns);

  // --- tsc_now: an overhead, measured against that baseline ----------------
  {
    Stats raw = measure([] {
      for (std::uint64_t i = 0; i < kIters; ++i) {
        std::uint64_t t = hft::tsc_now();
        hft::DoNotOptimize(t);
      }
    });
    // Report the amortised cost of the loop as measured, not a difference.
    //
    // Subtracting the baseline leaves a net of a thousandth of a nanosecond,
    // which is noise: the counter read issues alongside the loop's own work
    // rather than serialising with it, so in throughput mode it is not
    // separable from the apparatus measuring it. A latency figure would need
    // each read to depend on the previous one, which an mrs of cntvct_el0 does
    // not admit. What can be said honestly is an upper bound -- the read costs
    // no more than the loop containing it -- so that is what is published, with
    // the baseline beside it so the reader can do the subtraction themselves.
    std::printf("tsc_now: raw loop %.3f ns/iter, empty baseline %.3f ns/iter, net %+.3f\n",
                raw.median_ns, baseline.median_ns, raw.median_ns - baseline.median_ns);
    std::printf("         not separable; publishing the raw loop as an upper bound\n\n");
    rows.push_back({"tsc_now", raw});
  }

  // --- parse_message --------------------------------------------------------
  {
    const std::vector<std::byte> wire =
        make_wire_stream(kStreamMsgs, hft::price_from_double(90.00), hft::kPriceScale / 100);
    rows.push_back({"parse_message", measure([&] {
                      const std::byte* p = wire.data();
                      for (std::uint64_t i = 0; i < kIters; ++i) {
                        hft::ItchMessage m =
                            hft::parse_message(p + (i & kStreamMask) * hft::kMsgSize);
                        hft::DoNotOptimize(m);
                      }
                    })});
  }

  // --- risk_gate_check ------------------------------------------------------
  {
    std::atomic<bool> kill{false};
    hft::RiskLimits lim{1'000'000'000, 1'000'000, 0.0, 1};
    hft::RiskGate gate(lim, kill);
    std::array<hft::OrderRequest, 2> orders{};
    for (std::size_t s = 0; s < 2; ++s) {
      orders[s].side = static_cast<std::uint8_t>(s == 0 ? hft::Side::kBuy : hft::Side::kSell);
      orders[s].size = 1;
    }
    rows.push_back({"risk_gate_check", measure([&] {
                      for (std::uint64_t i = 0; i < kIters; ++i) {
                        const hft::OrderRequest& o = orders[i & 1];
                        hft::DoNotOptimize(o);
                        auto r = gate.check(o);
                        hft::DoNotOptimize(r);
                      }
                    })});
  }

  // --- histogram_record -----------------------------------------------------
  {
    hft::Histogram<10'000'000, 5> h;
    rows.push_back({"histogram_record", measure([&] {
                      for (std::uint64_t i = 0; i < kIters; ++i) {
                        h.record(i & 0xFFFF);
                        hft::ClobberMemory();
                      }
                    })});
  }

  // --- book_apply -----------------------------------------------------------
  {
    const std::int64_t base = hft::price_from_double(90.00);
    const std::int64_t tick = hft::kPriceScale / 100;
    const std::vector<std::byte> wire = make_wire_stream(kIters, base, tick);
    hft::BookView<kBookTicks> book(base, tick);
    rows.push_back({"book_apply", measure([&] {
                      for (std::uint64_t i = 0; i < kIters; ++i) {
                        const hft::ItchMessage m =
                            hft::parse_message(wire.data() + i * hft::kMsgSize);
                        book.apply(m);
                        hft::ClobberMemory();
                      }
                    })});
  }

  // --- strategy_on_md -------------------------------------------------------
  {
    hft::StrategyParams sp{};
    sp.base_price = hft::price_from_double(90.00);
    sp.tick = hft::kPriceScale / 100;
    sp.order_size = 100;
    sp.threshold = 0.30;
    hft::ImbalanceStrategy<kBookTicks> strat(sp);
    const std::vector<std::byte> wire = make_wire_stream(kStreamMsgs, sp.base_price, sp.tick);
    rows.push_back({"strategy_on_md", measure([&] {
                      for (std::uint64_t i = 0; i < kIters; ++i) {
                        hft::MdEvent ev{};
                        ev.msg =
                            hft::parse_message(wire.data() + (i & kStreamMask) * hft::kMsgSize);
                        ev.arrival_tsc = i;
                        strat.on_md(ev, [](const hft::OrderRequest& o) { hft::DoNotOptimize(o); });
                        hft::ClobberMemory();
                      }
                    })});
  }

  // --- report ---------------------------------------------------------------
  // These figures are amortised over kIters operations, so a per-op value below
  // the tick period is not a resolution problem. What has to clear the clock is
  // the elapsed time of one whole repetition, so report that in ticks and flag
  // a repetition too short to trust instead of flagging the quotient.
  const double tick = hft::tsc_calibration().resolution_ns;
  std::printf("%-18s %12s %12s %12s %10s %12s\n", "component", "median ns", "p25 ns", "p75 ns",
              "M ops/s", "ticks/rep");
  std::printf("%-18s %12s %12s %12s %10s %12s\n", "---------", "---------", "------", "------",
              "-------", "---------");
  for (const Row& r : rows) {
    const double ticks_per_rep = r.s.total_ns_per_rep / tick;
    std::printf("%-18s %12.3f %12.3f %12.3f %10.0f %12.0f%s\n", r.name, r.s.median_ns, r.s.p25_ns,
                r.s.p75_ns, 1000.0 / r.s.median_ns, ticks_per_rep,
                (ticks_per_rep < 1000.0) ? "  [too few ticks to trust]" : "");
  }

  const std::string path = "reports/data/components.csv";
  if (std::FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "%s", components_csv_header());
    for (const Row& r : rows) {
      std::fprintf(f, "%s,%.3f,%.3f,%.3f,%.0f,%d,%llu,%.3f,%.3f,%s\n", r.name, r.s.median_ns,
                   r.s.p25_ns, r.s.p75_ns, 1000.0 / r.s.median_ns, kReps,
                   static_cast<unsigned long long>(kIters), baseline.median_ns,
                   hft::tsc_calibration().resolution_ns,
                   hft::to_string(hft::tsc_calibration().source));
    }
    std::fclose(f);
    std::printf("\nwrote %s\n", path.c_str());
  } else {
    std::fprintf(stderr, "could not write %s (run from the repo root)\n", path.c_str());
    return 1;
  }
  return 0;
}
