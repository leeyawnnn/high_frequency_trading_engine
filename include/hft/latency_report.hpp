// Pretty-printer for latency histograms and the full per-stage engine report.
// Reporting only — never on the hot path.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "hft/histogram.hpp"

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
      std::fprintf(f, "%llu,%llu\n",
                   static_cast<unsigned long long>(h.bucket_upper_bound(i)),
                   static_cast<unsigned long long>(c));
  }
  std::fclose(f);
}

// Append one stage's percentile summary row to an open CSV.
template <std::uint64_t H, unsigned P>
void write_summary_row(std::FILE* f, const char* stage, const Histogram<H, P>& h) {
  std::fprintf(f, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%.0f\n", stage,
               static_cast<unsigned long long>(h.count()),
               static_cast<unsigned long long>(h.percentile(50.0)),
               static_cast<unsigned long long>(h.percentile(90.0)),
               static_cast<unsigned long long>(h.percentile(99.0)),
               static_cast<unsigned long long>(h.percentile(99.9)),
               static_cast<unsigned long long>(h.max()), h.mean());
}

template <std::uint64_t H, unsigned P>
void print_latency_row(const char* label, const Histogram<H, P>& h) {
  if (h.count() == 0) {
    std::printf("  %-18s  (no samples)\n", label);
    return;
  }
  std::printf("  %-18s n=%-9llu  p50=%-6llu p90=%-6llu p99=%-7llu p99.9=%-7llu "
              "max=%-8llu mean=%.0f\n",
              label,
              static_cast<unsigned long long>(h.count()),
              static_cast<unsigned long long>(h.percentile(50.0)),
              static_cast<unsigned long long>(h.percentile(90.0)),
              static_cast<unsigned long long>(h.percentile(99.0)),
              static_cast<unsigned long long>(h.percentile(99.9)),
              static_cast<unsigned long long>(h.max()),
              h.mean());
}

// Print the four-stage report. Each argument is a latency histogram in ns.
template <typename HFeed, typename HStrat, typename HGw, typename HE2E, typename HRt>
void print_engine_report(const HFeed& feed, const HStrat& strat, const HGw& gw,
                         const HE2E& e2e, const HRt& round_trip) {
  std::printf("\n==================== latency report (nanoseconds) ====================\n");
  print_latency_row("feed handler", feed);    // socket recv -> enqueue
  print_latency_row("strategy", strat);        // md dequeue -> decision
  print_latency_row("gateway", gw);            // order dequeue -> wire
  std::printf("  ------------------------------------------------------------------\n");
  print_latency_row("END-TO-END", e2e);        // feed arrival -> order on wire
  print_latency_row("round trip", round_trip); // feed arrival -> fill received
  std::printf("======================================================================\n");
}

}  // namespace hft
