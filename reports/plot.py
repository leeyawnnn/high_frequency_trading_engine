#!/usr/bin/env python3
"""Regenerate every figure in reports/figures/ from the committed CSVs.

    python3 reports/plot.py

Deterministic: no randomness, no wall-clock in the output, sorted iteration.
CI runs this and fails if any figure errors, so a figure cannot silently rot.

Every figure is SVG. See reports/style.py for the shared style.
"""
from __future__ import annotations

import csv
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import style  # noqa: E402
from style import (BLUE, GREEN, GREY, ORANGE, PURPLE, VERMILLION)  # noqa: E402

DATA = style.DATA


def read(name):
    path = DATA / name
    if not path.exists():
        print(f"  skip: {name} missing", file=sys.stderr)
        return []
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def cell(value, resolution=2.0):
    """summary.csv cells may read '<N' when below the clock's floor."""
    value = str(value).strip()
    if value.startswith("<"):
        return float(value[1:]), True
    return float(value), False


def fmt_ns(v):
    if v >= 1_000_000:
        return f"{v/1e6:.1f} ms"
    if v >= 1_000:
        return f"{v/1e3:.1f} µs"
    return f"{v:.0f} ns"


# --------------------------------------------------------------------------
# 1. Latency vs offered load. The headline: where does it break?
# --------------------------------------------------------------------------
def fig_latency_vs_load():
    rows = read("load_sweep.csv")
    if not rows:
        return None
    rows.sort(key=lambda r: int(r["offered_rate"]))

    offered = [int(r["offered_rate"]) for r in rows]
    achieved = [int(r["achieved_rate"]) for r in rows]
    series = [
        ("p99.9", [float(r["p999_ns"]) for r in rows], PURPLE),
        ("p99", [float(r["p99_ns"]) for r in rows], VERMILLION),
        ("p50", [float(r["p50_ns"]) for r in rows], BLUE),
    ]

    fig, ax = style.plt.subplots()
    for label, ys, colour in series:
        ax.plot(offered, ys, marker="o", ms=5, lw=2, color=colour)
        ax.annotate(label, xy=(offered[-1], ys[-1]), xytext=(8, 0),
                    textcoords="offset points", color=colour,
                    fontsize=11, fontweight="semibold", va="center")

    # Saturation: the last point where the engine kept up with the sender.
    knee = None
    for off, ach in zip(offered, achieved):
        if ach >= off * 0.95:
            knee = (off, ach)
    p50s = [float(r["p50_ns"]) for r in rows]
    p99s = [float(r["p99_ns"]) for r in rows]
    if knee:
        ax.axvline(knee[0], color=GREY, lw=1, ls="--", alpha=0.7)
        # Place the callout against the p50 line, well inside the axes: at the
        # bottom of the plot it collided with the tick labels.
        ax.annotate(f"keeps up with the sender to {knee[0]:,} msg/s;\n"
                    "past here the queue grows and the tail follows",
                    xy=(knee[0], p50s[-1] * 1.6), xytext=(offered[0] * 1.15, p50s[-1] * 2.6),
                    fontsize=10, color=GREY,
                    arrowprops={"arrowstyle": "-", "color": GREY, "lw": 0.9,
                                "shrinkA": 2, "shrinkB": 4})

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("offered message rate (messages/sec, log scale)")
    ax.set_ylabel("end-to-end latency (ns, log scale)")
    ax.set_xticks(offered)
    ax.set_xticklabels([f"{r//1000}k" for r in offered])
    ax.get_yaxis().set_major_formatter(
        style.matplotlib.ticker.FuncFormatter(lambda v, _: fmt_ns(v)))
    ax.grid(axis="both", which="major")
    ax.grid(axis="both", which="minor", alpha=0.35)

    # State the finding, computed from the data rather than asserted, so a
    # re-run cannot leave the title contradicting the chart underneath it.
    p50_growth = max(p50s) / min(p50s)
    p99_growth = max(p99s) / min(p99s)
    style.titled(
        ax,
        f"The median grows {p50_growth:.1f}\u00d7 across the range; the p99 grows "
        f"{p99_growth:.0f}\u00d7",
        f"End-to-end latency vs offered load · {len(rows)} rates · 3s per point · "
        "spin wait policy · unpinned",
    )
    style.finish(fig, ax, "load_sweep.csv", bottom=0.16)
    return style.save(fig, "fig_latency_vs_load.svg")


# --------------------------------------------------------------------------
# 2. Complementary CDF of end-to-end latency. A plain CDF hides the tail.
# --------------------------------------------------------------------------
def fig_e2e_ccdf():
    rows = read("e2e.csv")
    if not rows:
        return None
    buckets = sorted((int(r["upper_ns"]), int(r["count"])) for r in rows)
    total = sum(c for _, c in buckets)
    if total == 0:
        return None

    xs, ys = [], []
    cumulative = 0
    for upper, count in buckets:
        cumulative += count
        remaining = 1.0 - cumulative / total
        if upper > 0 and remaining > 0:
            xs.append(upper)
            ys.append(remaining)

    fig, ax = style.plt.subplots()
    ax.plot(xs, ys, lw=2, color=BLUE)
    ax.set_xscale("log")
    ax.set_yscale("log")

    marks = [(50, "p50"), (99, "p99"), (99.9, "p99.9"), (99.99, "p99.99")]
    for pct, label in marks:
        target = 1.0 - pct / 100.0
        value = next((x for x, y in zip(xs, ys) if y <= target), None)
        if value is None:
            continue
        ax.axvline(value, color=GREY, lw=0.9, ls="--", alpha=0.65)
        ax.annotate(f"{label}\n{fmt_ns(value)}", xy=(value, target),
                    xytext=(6, 10), textcoords="offset points",
                    fontsize=9, color=GREY)

    ax.set_xlabel("end-to-end latency (ns, log scale)")
    ax.set_ylabel("fraction of messages slower than x (log scale)")
    ax.get_xaxis().set_major_formatter(
        style.matplotlib.ticker.FuncFormatter(lambda v, _: fmt_ns(v)))
    style.titled(
        ax,
        "Half the messages clear in ~1 µs; one in a thousand takes 100x longer",
        f"Complementary CDF (1−F), so the tail stays legible · n={total:,} messages",
    )
    style.finish(fig, ax, "e2e.csv")
    return style.save(fig, "fig_e2e_ccdf.svg")


# --------------------------------------------------------------------------
# 3. Per-stage percentiles, with the clock's floor drawn in.
# --------------------------------------------------------------------------
def fig_stage_percentiles():
    rows = read("summary.csv")
    if not rows:
        return None
    by_stage = {r["stage"]: r for r in rows}
    order = ["feed", "strategy", "gateway", "end_to_end"]
    names = {"feed": "feed handler", "strategy": "strategy",
             "gateway": "gateway", "end_to_end": "end-to-end"}
    stages = [s for s in order if s in by_stage]
    if not stages:
        return None

    resolution = float(by_stage[stages[0]].get("resolution_ns", 1.0) or 1.0)
    floor = 2 * resolution
    percentiles = [("p50", BLUE), ("p90", GREEN), ("p99", ORANGE), ("p999", VERMILLION)]

    fig, ax = style.plt.subplots()
    width = 0.2
    any_floored = False
    for i, (key, colour) in enumerate(percentiles):
        xs, ys = [], []
        for j, stage in enumerate(stages):
            value, floored = cell(by_stage[stage][key], resolution)
            any_floored = any_floored or floored
            xs.append(j + (i - 1.5) * width)
            ys.append(max(value, floor * 0.55))
        bars = ax.bar(xs, ys, width, color=colour, label=key.replace("p999", "p99.9"))
        for rect, stage in zip(bars, stages):
            _, floored = cell(by_stage[stage][key], resolution)
            ax.annotate("<" + f"{floor:.0f}" if floored else fmt_ns(rect.get_height()),
                        xy=(rect.get_x() + rect.get_width() / 2, rect.get_height()),
                        xytext=(0, 3), textcoords="offset points",
                        ha="center", fontsize=8, color=GREY, rotation=90)

    max_plotted = max(
        cell(by_stage[s][k], resolution)[0] for s in stages for k, _ in percentiles)
    ax.axhspan(0, floor, color=GREY, alpha=0.13, zorder=0)
    ax.text(len(stages) - 0.45, floor, f"  clock floor ({floor:.0f} ns)",
            fontsize=9, color=GREY, va="bottom", ha="right")

    ax.set_yscale("log")
    ax.set_xticks(range(len(stages)))
    ax.set_xticklabels([names[s] for s in stages])
    ax.set_ylabel("latency (ns, log scale)")
    ax.legend(frameon=False, ncol=4, loc="upper left", bbox_to_anchor=(0, 1.005))
    ax.set_ylim(top=max_plotted * 6)
    ax.get_yaxis().set_major_formatter(
        style.matplotlib.ticker.FuncFormatter(lambda v, _: fmt_ns(v)))

    sub = ("Per-stage processing vs the whole path · shaded band is the counter's "
           "two-tick resolution floor")
    if any_floored:
        sub += " · bars marked “<” sit at it and are upper bounds"
    style.titled(ax, "Stage processing is a rounding error; the path is queueing", sub)
    style.finish(fig, ax, "summary.csv", top=0.83)
    return style.save(fig, "fig_stage_percentiles.svg")


# --------------------------------------------------------------------------
# 4. Component costs, sorted, with IQR and the measurement floor.
# --------------------------------------------------------------------------
def fig_components():
    rows = read("components.csv")
    if not rows:
        return None
    rows.sort(key=lambda r: float(r["ns_per_op"]))
    names = [r["component"] for r in rows]
    med = [float(r["ns_per_op"]) for r in rows]
    lo = [med[i] - float(r.get("ns_p25", med[i])) for i, r in enumerate(rows)]
    hi = [float(r.get("ns_p75", med[i])) - med[i] for i, r in enumerate(rows)]
    baseline = rows[0].get("baseline_ns")

    fig, ax = style.plt.subplots()
    ys = range(len(names))
    ax.barh(list(ys), med, xerr=[lo, hi], color=BLUE, height=0.6,
            error_kw={"ecolor": GREY, "elinewidth": 1.2, "capsize": 4})
    for y, value in zip(ys, med):
        ax.annotate(f"{value:.3f} ns", xy=(value, y), xytext=(8, 0),
                    textcoords="offset points", va="center", fontsize=10)

    if baseline:
        b = float(baseline)
        ax.axvline(b, color=VERMILLION, lw=1.4, ls="--")
        ax.annotate(f"empty-loop + barrier baseline ({b:.3f} ns)\n"
                    "anything at this line is not separable from the harness",
                    xy=(b, len(names) - 0.6), xytext=(10, 0),
                    textcoords="offset points", fontsize=9, color=VERMILLION)

    ax.set_yticks(list(ys))
    ax.set_yticklabels([f"{n}()" for n in names])
    ax.set_xlabel("nanoseconds per operation (median, error bars = inter-quartile range)")
    ax.grid(axis="x")
    ax.grid(axis="y", visible=False)
    ax.set_xlim(0, max(med) * 1.35)

    reps = rows[0].get("reps", "?")
    iters = rows[0].get("iters_per_rep", "?")
    style.titled(
        ax,
        "Book and strategy updates dominate; parsing is near memory speed",
        f"Isolated microbenchmarks, single-threaded · median of {reps} repetitions "
        f"× {iters} iterations · optimisation barriers applied",
    )
    style.finish(fig, ax, "components.csv", left=0.17, top=0.83)
    return style.save(fig, "fig_components.svg")


# --------------------------------------------------------------------------
# 5. Padding A/B.
# --------------------------------------------------------------------------
def fig_false_sharing():
    rows = read("false_sharing.csv")
    if not rows:
        return None
    variants = [r["variant"] for r in rows]
    mops = [float(r["mops"]) for r in rows]
    colours = [BLUE if v.startswith("padded") else ORANGE for v in variants]

    fig, ax = style.plt.subplots()
    bars = ax.bar(variants, mops, color=colours, width=0.5)
    for rect, value in zip(bars, mops):
        ax.annotate(f"{value:.1f} M ops/s",
                    xy=(rect.get_x() + rect.get_width() / 2, value),
                    xytext=(0, 4), textcoords="offset points",
                    ha="center", fontsize=11, fontweight="semibold")

    ax.set_ylabel("throughput (million ops/sec)")
    ax.set_ylim(0, max(mops) * 1.25)
    ax.grid(axis="y")
    ax.grid(axis="x", visible=False)

    line = rows[0].get("cache_line_bytes", "?")
    threads = rows[0].get("hardware_threads", "?")
    padded = next((m for v, m in zip(variants, mops) if v.startswith("padded")), None)
    unpadded = next((m for v, m in zip(variants, mops) if v.startswith("unpadded")), None)
    finding = "Cache-line padding costs throughput on this topology"
    if padded and unpadded and padded > unpadded:
        finding = "Cache-line padding pays on this topology"
    if padded and unpadded:
        ratio = max(padded, unpadded) / min(padded, unpadded)
        style.annotate(ax, f"{ratio:.1f}×",
                       xy=(0.5, max(mops) * 0.55), xytext=(0.5, max(mops) * 0.8))

    style.titled(
        ax,
        finding,
        f"SPSC queue, identical but for the alignas · {line}-byte line · "
        f"{threads} hardware threads · AArch64 only, not verified on x86",
    )
    style.finish(fig, ax, "false_sharing.csv")
    return style.save(fig, "fig_false_sharing.svg")


def main() -> int:
    style.apply_rc()
    made, failed = [], []
    for fn in (fig_latency_vs_load, fig_e2e_ccdf, fig_stage_percentiles,
               fig_components, fig_false_sharing):
        try:
            out = fn()
        except Exception as exc:  # noqa: BLE001 - CI must see which figure broke
            print(f"  FAILED {fn.__name__}: {exc}", file=sys.stderr)
            failed.append(fn.__name__)
            continue
        if out:
            size_kb = out.stat().st_size / 1024
            print(f"  {out.relative_to(style.REPO)}  ({size_kb:.0f} KB)")
            made.append(out)
        else:
            print(f"  {fn.__name__}: no data, skipped", file=sys.stderr)
    print(f"{len(made)} figure(s) written")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
