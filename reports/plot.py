#!/usr/bin/env python3
"""Generate the report figures from the CSVs the engine emits.

Inputs  (reports/data/):
  feed.csv strategy.csv gateway.csv e2e.csv round_trip.csv  -- histogram buckets (upper_ns,count)
  summary.csv                                               -- per-stage percentiles
  components.csv                                            -- component microbench (ns/op, Mops)
  false_sharing.csv                                         -- padded vs unpadded SPSC throughput

Outputs (reports/figures/):
  fig_e2e_cdf.png          fig_stage_percentiles.png
  fig_components.png       fig_false_sharing.png

Usage:  python3 reports/plot.py
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
FIGS = os.path.join(HERE, "figures")
os.makedirs(FIGS, exist_ok=True)


def load_hist(name):
    """Return (upper_ns array, count array) for a histogram CSV."""
    ups, cnts = [], []
    with open(os.path.join(DATA, name)) as f:
        for row in csv.DictReader(f):
            ups.append(float(row["upper_ns"]))
            cnts.append(float(row["count"]))
    return np.array(ups), np.array(cnts)


def load_summary():
    rows = {}
    with open(os.path.join(DATA, "summary.csv")) as f:
        for r in csv.DictReader(f):
            rows[r["stage"]] = {k: float(v) for k, v in r.items() if k != "stage"}
    return rows


# ---------------------------------------------------------------------------
# Figure 1: end-to-end latency CDF (log-x), with percentile markers.
# ---------------------------------------------------------------------------
def fig_e2e_cdf():
    ups, cnts = load_hist("e2e.csv")
    order = np.argsort(ups)
    ups, cnts = ups[order], cnts[order]
    cdf = np.cumsum(cnts) / cnts.sum()

    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.plot(ups, cdf * 100, lw=2, color="#1f77b4")
    ax.set_xscale("log")
    ax.set_xlabel("end-to-end latency  (nanoseconds, log scale)")
    ax.set_ylabel("percentile  (%)")
    ax.set_title("End-to-end latency CDF  (feed arrival → order ready for wire)")
    ax.grid(True, which="both", ls=":", alpha=0.5)

    s = load_summary()["end_to_end"]
    for p, key, c in [(50, "p50", "#2ca02c"), (90, "p90", "#ff7f0e"), (99, "p99", "#d62728")]:
        x = s[key]
        ax.axvline(x, color=c, ls="--", alpha=0.8)
        ax.annotate(f"p{p} = {x/1000:.2f} µs", xy=(x, p),
                    xytext=(x * 1.15, p - 12), color=c, fontsize=9,
                    arrowprops=dict(arrowstyle="->", color=c, alpha=0.7))
    ax.axhspan(0, 100, xmin=0, xmax=0, alpha=0)  # keep limits sane
    ax.set_ylim(0, 102)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "fig_e2e_cdf.png"), dpi=130)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Figure 2: per-stage percentile comparison (grouped bars, log-y).
# ---------------------------------------------------------------------------
def fig_stage_percentiles():
    s = load_summary()
    stages = ["feed", "strategy", "gateway", "end_to_end"]
    labels = ["feed\nhandler", "strategy", "gateway", "END-TO-END"]
    pcts = ["p50", "p90", "p99"]
    colors = ["#2ca02c", "#ff7f0e", "#d62728"]

    x = np.arange(len(stages))
    w = 0.26
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for i, (p, c) in enumerate(zip(pcts, colors)):
        vals = [max(s[st][p], 1) for st in stages]  # clamp 0 -> 1ns for log
        bars = ax.bar(x + (i - 1) * w, vals, w, label=p, color=c)
        for b, v in zip(bars, [s[st][p] for st in stages]):
            ax.annotate(f"{v:.0f}", xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                        xytext=(0, 2), textcoords="offset points",
                        ha="center", fontsize=7)
    ax.set_yscale("log")
    ax.set_ylabel("latency  (nanoseconds, log scale)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_title("Per-stage latency percentiles  (compute is tens of ns; e2e tail is scheduling)")
    ax.axhline(1000, color="grey", ls=":", alpha=0.7)
    ax.annotate("1 µs", xy=(len(stages) - 0.5, 1000), color="grey", fontsize=8, va="bottom")
    ax.legend(title="percentile")
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "fig_stage_percentiles.png"), dpi=130)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Figure 3: component microbenchmark throughput.
# ---------------------------------------------------------------------------
def fig_components():
    names, ns, mops = [], [], []
    with open(os.path.join(DATA, "components.csv")) as f:
        for r in csv.DictReader(f):
            names.append(r["component"])
            ns.append(float(r["ns_per_op"]))
            mops.append(float(r["mops"]))
    order = np.argsort(ns)
    names = [names[i] for i in order]
    ns = [ns[i] for i in order]

    fig, ax = plt.subplots(figsize=(8, 4.5))
    bars = ax.barh(names, ns, color="#1f77b4")
    for b, v in zip(bars, ns):
        ax.annotate(f"{v:.2f} ns", xy=(b.get_width(), b.get_y() + b.get_height() / 2),
                    xytext=(4, 0), textcoords="offset points", va="center", fontsize=9)
    ax.set_xlabel("cost per operation  (nanoseconds)")
    ax.set_title("Hot-path component cost (M4 dev box, isolated microbenchmarks)")
    ax.grid(True, axis="x", ls=":", alpha=0.5)
    ax.set_xlim(0, max(ns) * 1.25)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "fig_components.png"), dpi=130)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Figure 4: the false-sharing discovery (padded vs unpadded on Apple Silicon).
# ---------------------------------------------------------------------------
def fig_false_sharing():
    variants, mops = [], []
    with open(os.path.join(DATA, "false_sharing.csv")) as f:
        for r in csv.DictReader(f):
            variants.append(r["variant"])
            mops.append(float(r["mops"]))
    fig, ax = plt.subplots(figsize=(8, 4.5))
    colors = ["#2ca02c", "#d62728"]
    bars = ax.bar(variants, mops, color=colors[: len(variants)])
    for b, v in zip(bars, mops):
        ax.annotate(f"{v:.0f} M ops/s", xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                    xytext=(0, 3), textcoords="offset points", ha="center", fontsize=10)
    ax.set_ylabel("cross-thread throughput  (M ops/s)")
    ax.set_title("Surprise: on Apple M4, cache-line padding HURT the SPSC ~%.1fx\n"
                 "(textbook x86 result is the opposite — measure your target!)"
                 % (mops[0] / mops[1]))
    ax.set_ylim(0, max(mops) * 1.2)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "fig_false_sharing.png"), dpi=130)
    plt.close(fig)


if __name__ == "__main__":
    fig_e2e_cdf()
    fig_stage_percentiles()
    fig_components()
    fig_false_sharing()
    print("wrote figures to", FIGS)
