// Pretty-printer for latency histograms and the full per-stage engine report.
// Reporting only — never on the hot path.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "hft/histogram.hpp"
#include "hft/tsc.hpp"

namespace hft {

// Dump a histogram's non-empty buckets as CSV (upper_ns,count) for plotting.
template <std::uint64_t H, unsigned P>
void dump_histogram_csv(const std::string& path, const Histogram<H, P>& h) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "upper_ns,count\n");
  for (std::size_t i = 0; i < h.counts_len(); ++i) {
    const std::uint64_t c = h.raw_count(i);
    if (c)
      std::fprintf(f, "%llu,%llu\n", static_cast<unsigned long long>(h.bucket_upper_bound(i)),
                   static_cast<unsigned long long>(c));
  }
  std::fclose(f);
}

// Header for the summary CSV. The clock columns are not decoration: a latency
// table is uninterpretable without the resolution it was measured at, and
// keeping them in the same row as the numbers means the two cannot be
// separated by a copy-paste.
inline const char* summary_csv_header() noexcept {
  return "stage,n,p50,p90,p99,p999,max,mean,resolution_ns,clock_source\n";
}

// Append one stage's percentile summary row to an open CSV.
//
// Percentile cells below the clock's resolution floor are written as "<N"
// rather than as a number. A p50 of 0 from a counter that ticks every 41.67 ns
// does not mean the stage took no time; it means the stage finished inside one
// tick, and writing 0 invites the reader to believe the former.
template <std::uint64_t H, unsigned P>
void write_summary_row(std::FILE* f, const char* stage, const Histogram<H, P>& h) {
  const TscCalibration& cal = tsc_calibration();
  char p50[32];
  char p90[32];
  char p99[32];
  char p999[32];
  char mx[32];
  format_ns(p50, sizeof p50, h.percentile(50.0));
  format_ns(p90, sizeof p90, h.percentile(90.0));
  format_ns(p99, sizeof p99, h.percentile(99.0));
  format_ns(p999, sizeof p999, h.percentile(99.9));
  format_ns(mx, sizeof mx, h.max());
  std::fprintf(f, "%s,%llu,%s,%s,%s,%s,%s,%.0f,%.3f,%s\n", stage,
               static_cast<unsigned long long>(h.count()), p50, p90, p99, p999, mx, h.mean(),
               cal.resolution_ns, to_string(cal.source));
}

template <std::uint64_t H, unsigned P>
void print_latency_row(const char* label, const Histogram<H, P>& h) {
  if (h.count() == 0) {
    std::printf("  %-18s  (no samples)\n", label);
    return;
  }
  char p50[32];
  char p90[32];
  char p99[32];
  char p999[32];
  char mx[32];
  format_ns(p50, sizeof p50, h.percentile(50.0));
  format_ns(p90, sizeof p90, h.percentile(90.0));
  format_ns(p99, sizeof p99, h.percentile(99.0));
  format_ns(p999, sizeof p999, h.percentile(99.9));
  format_ns(mx, sizeof mx, h.max());
  std::printf(
      "  %-18s n=%-9llu  p50=%-7s p90=%-7s p99=%-8s p99.9=%-8s "
      "max=%-9s mean=%.0f\n",
      label, static_cast<unsigned long long>(h.count()), p50, p90, p99, p999, mx, h.mean());
}

// Print the four-stage report. Each argument is a latency histogram in ns.
template <typename HFeed, typename HStrat, typename HGw, typename HE2E, typename HRt>
void print_engine_report(const HFeed& feed, const HStrat& strat, const HGw& gw, const HE2E& e2e,
                         const HRt& round_trip) {
  std::printf("\n==================== measurement conditions ====================\n");
  print_clock_report(stdout);
  std::printf("\n==================== latency report (nanoseconds) ====================\n");
  print_latency_row("feed handler", feed);  // socket recv -> enqueue
  print_latency_row("strategy", strat);     // md dequeue -> decision
  print_latency_row("gateway", gw);         // order dequeue -> wire
  std::printf("  ------------------------------------------------------------------\n");
  print_latency_row("END-TO-END", e2e);         // feed arrival -> order on wire
  print_latency_row("round trip", round_trip);  // feed arrival -> fill received
  std::printf("======================================================================\n");
}

}  // namespace hft
