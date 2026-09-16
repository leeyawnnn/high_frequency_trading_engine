#!/usr/bin/env bash
#
# Regenerate every measured artifact in reports/data/ from a clean build.
#
# One command, so that the numbers in the README can be reproduced rather than
# trusted. Each artifact gets a sibling .meta.json recording the exact command,
# the commit it was produced from, the UTC timestamp and the machine, because a
# latency figure without its machine is not a result.
#
# Usage:
#   scripts/measure.sh [build-dir]
#
# The latency run uses --wait spin deliberately. That is the low-latency policy
# and it requires a machine with a free core per stage; it is not the engine's
# default. Run this on an otherwise idle machine or the tail percentiles will
# record your desktop rather than the engine.
set -euo pipefail

BUILD_DIR="${1:-build}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DURATION_MS=4000
RATE=100000
BATCH=4
WAIT_POLICY=spin

if [ ! -x "$BUILD_DIR/tools/latency_report" ]; then
  echo "error: $BUILD_DIR/tools/latency_report not found." >&2
  echo "Build first:  cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j" >&2
  exit 1
fi

COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
DIRTY=""
git diff --quiet 2>/dev/null || DIRTY=" (working tree dirty)"
STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

if [ "$(uname -s)" = "Darwin" ]; then
  CPU="$(sysctl -n machdep.cpu.brand_string)"
  CORES="$(sysctl -n hw.ncpu)"
  OSVER="macOS $(sw_vers -productVersion) ($(uname -m))"
else
  CPU="$(awk -F': ' '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null || echo unknown)"
  CORES="$(getconf _NPROCESSORS_ONLN)"
  OSVER="$(uname -sr) ($(uname -m))"
fi
LOAD="$(uptime | sed 's/.*load averages*: //')"

# $1 = artifact path, $2 = command string
write_meta() {
  cat > "${1}.meta.json" <<JSON
{
  "artifact": "$1",
  "command": "$2",
  "git_commit": "${COMMIT}${DIRTY}",
  "generated_utc": "${STAMP}",
  "machine": {
    "cpu": "${CPU}",
    "logical_cores": ${CORES},
    "os": "${OSVER}",
    "load_average_at_start": "${LOAD}"
  },
  "note": "Synthetic market data from the bundled SimExchange, not a live venue. See README 'Measurement methodology and its limits'."
}
JSON
}

echo "commit ${COMMIT}${DIRTY}"
echo "machine ${CPU} / ${CORES} cores / ${OSVER}"
echo "load at start: ${LOAD}"
echo

LAT_CMD="$BUILD_DIR/tools/latency_report --duration-ms $DURATION_MS --rate $RATE --batch $BATCH --wait $WAIT_POLICY --csv-dir reports/data"
echo "==> $LAT_CMD"
$LAT_CMD 2>&1 | tee reports/data/latency_run.txt
for f in summary feed strategy gateway e2e round_trip; do
  [ -f "reports/data/$f.csv" ] && write_meta "reports/data/$f.csv" "$LAT_CMD"
done
write_meta "reports/data/latency_run.txt" "$LAT_CMD"

echo
COMP_CMD="$BUILD_DIR/benchmarks/bench_components"
echo "==> $COMP_CMD"
$COMP_CMD
write_meta "reports/data/components.csv" "$COMP_CMD"

echo
FS_CMD="$BUILD_DIR/benchmarks/bench_false_sharing"
echo "==> $FS_CMD"
$FS_CMD
write_meta "reports/data/false_sharing.csv" "$FS_CMD"

echo
echo "==> rendering tables"
python3 tools/render_tables.py

echo
echo "done. Regenerate figures with: python3 reports/plot.py"
