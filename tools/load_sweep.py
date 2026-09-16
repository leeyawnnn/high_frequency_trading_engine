#!/usr/bin/env python3
"""Sweep offered load and record how latency degrades.

A single percentile table answers "how fast is it when nothing is wrong".
The question that decides whether a system is usable is "where does it break",
and that needs latency as a function of offered rate: the point where the
curve turns up is the capacity, and everything past it is queue growth rather
than processing cost.

Runs the engine at a series of message rates and collects p50/p99/p99.9 plus
achieved throughput and drop count at each, writing reports/data/load_sweep.csv.

Usage:  python3 tools/load_sweep.py [build-dir]
"""
from __future__ import annotations

import csv
import datetime as _dt
import json
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "reports" / "data" / "load_sweep.csv"

# Offered rates in messages/sec. The top of the range is past what the loopback
# path sustains, which is the point: the curve has to include the knee.
RATES = [10_000, 25_000, 50_000, 100_000, 200_000, 400_000, 800_000, 1_600_000]
DURATION_MS = 3000
BATCH = 4
WAIT = "spin"


def machine_info() -> dict:
    """The real CPU model, not platform.processor()'s "arm".

    A provenance block whose machine field reads "arm" is not much better than
    no machine field: these numbers differ several-fold across AArch64 parts.
    """
    import os
    cpu = platform.processor() or platform.machine()
    if platform.system() == "Darwin":
        try:
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                 capture_output=True, text=True,
                                 check=True).stdout.strip() or cpu
        except (OSError, subprocess.CalledProcessError):
            pass
    elif platform.system() == "Linux":
        try:
            for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
        except OSError:
            pass
    return {
        "cpu": cpu,
        "logical_cores": os.cpu_count(),
        "os": f"{platform.system()} {platform.release()} ({platform.machine()})",
    }


def parse_cell(value: str, resolution_ns: float) -> float:
    """Turn a summary.csv cell into a number.

    Cells below the clock's floor are written '<N' rather than a figure. For
    plotting they become the floor itself, which is an upper bound on the true
    value and is labelled as such on the figure.
    """
    value = value.strip()
    if value.startswith("<"):
        return float(value[1:])
    return float(value)


def run_one(binary: pathlib.Path, rate: int) -> dict | None:
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [
            str(binary),
            "--duration-ms", str(DURATION_MS),
            "--rate", str(rate),
            "--batch", str(BATCH),
            "--wait", WAIT,
            "--csv-dir", tmp,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"  rate={rate}: FAILED rc={proc.returncode}", file=sys.stderr)
            print(proc.stderr[:500], file=sys.stderr)
            return None

        sent = processed = 0
        m = re.search(r"exchange sent (\d+) feed msgs, engine processed (\d+)", proc.stdout)
        if m:
            sent, processed = int(m.group(1)), int(m.group(2))

        summary = pathlib.Path(tmp) / "summary.csv"
        if not summary.exists():
            print(f"  rate={rate}: no summary.csv", file=sys.stderr)
            return None

        rows = {r["stage"]: r for r in csv.DictReader(summary.open(newline=""))}
        e2e = rows.get("end_to_end")
        if not e2e:
            return None
        res = float(e2e.get("resolution_ns", "1") or 1)
        achieved = processed / (DURATION_MS / 1000.0)
        return {
            "offered_rate": rate,
            "achieved_rate": round(achieved),
            "sent": sent,
            "processed": processed,
            "dropped": max(0, sent - processed),
            "n": e2e["n"],
            "p50_ns": parse_cell(e2e["p50"], res),
            "p99_ns": parse_cell(e2e["p99"], res),
            "p999_ns": parse_cell(e2e["p999"], res),
            "max_ns": parse_cell(e2e["max"], res),
            "resolution_ns": res,
            "clock_source": e2e.get("clock_source", "?"),
        }


def main() -> int:
    build = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build"
    binary = build / "tools" / "latency_report"
    if not binary.exists():
        print(f"error: {binary} not found. Build first.", file=sys.stderr)
        return 1

    print(f"sweeping {len(RATES)} rates x {DURATION_MS}ms, --wait {WAIT}")
    results = []
    for rate in RATES:
        print(f"  rate={rate:>9,} ...", end=" ", flush=True)
        row = run_one(binary, rate)
        if row:
            results.append(row)
            print(f"achieved {row['achieved_rate']:>9,}/s  "
                  f"p50={row['p50_ns']:>9,.0f}  p99={row['p99_ns']:>10,.0f} ns")
    if not results:
        print("error: no successful runs", file=sys.stderr)
        return 1

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(results[0].keys()))
        w.writeheader()
        w.writerows(results)
    print(f"wrote {OUT.relative_to(REPO)}")

    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                            text=True, cwd=REPO).stdout.strip() or "unknown"
    dirty = subprocess.run(["git", "diff", "--quiet"], cwd=REPO).returncode != 0
    meta = {
        "artifact": str(OUT.relative_to(REPO)),
        "command": f"python3 tools/load_sweep.py {build}",
        "git_commit": commit + (" (working tree dirty)" if dirty else ""),
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "machine": machine_info(),
        "note": "Synthetic market data over loopback UDP. Offered rate is what the "
                "sender attempted; achieved rate is what the engine processed.",
    }
    (OUT.parent / f"{OUT.name}.meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
