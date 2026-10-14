#!/usr/bin/env bash
# One-click full benchmark: 8 policies (baseline + 7 handover) x 5 repetitions x 3 tools
# (business / statistics / traceroute). Real-socket iperf loopback (0.1 s) and real
# traceroute over a synthetic 3-satellite + user/ground topology.
set -euo pipefail
cd "$(dirname "$0")"

BIN=${BIN:-build/bin/hypersim}
OUT=${OUT:-benchmark_runs}
POLICIES=${POLICIES:-none,random,min-access-delay,min-full-delay,max-min-visibility,min-orbit-hops,max-access-rate,min-lifetime}
TIMES=${TIMES:-5}

echo "== build =="
python3 build_gcc.py >/dev/null 2>&1 || { echo "build failed" >&2; exit 1; }

echo "== generate synthetic input (3 sats n0..n2 + user u1 + ground gw1, 3 x 10s slices) =="
rm -rf "$OUT"
mkdir -p "$OUT"
INPUT="$OUT/input"
python3 - "$INPUT" <<'PY'
import sys
sys.path.insert(0, "tests")
from fixtures import make
from pathlib import Path
root = Path(sys.argv[1])
def bi(a, b, d, bw):
    return [(a, b, d, bw, d * 299792.458), (b, a, d, bw, d * 299792.458)]
isl = bi(0, 1, 2000, 10000000) + bi(1, 2, 2000, 10000000)
gw  = bi(2, 4, 4000, 50000000)
s0 = isl + gw + bi(3, 0, 5000, 40000000) + bi(3, 1, 3000, 20000000)
s1 = list(s0)
s2 = isl + gw + bi(3, 0, 5000, 40000000)
make(root, [s0, s1, s2], ground=[('u1', 1), ('gw1', 2)])
PY

NPOL=$(printf '%s' "$POLICIES" | tr ',' '\n' | grep -c .)
echo "== benchmark: $NPOL policies x $TIMES times = $((NPOL * TIMES)) runs =="
"$BIN" "$INPUT" --policy "$POLICIES" --times "$TIMES" --output "$OUT/run"

echo "== verify =="
EXPECTED=$((NPOL * TIMES))
ACTUAL=$(find "$OUT/run" -name '*-manifest-*.json' | wc -l)
echo "manifests: $ACTUAL (expected $EXPECTED)"
[ "$ACTUAL" -eq "$EXPECTED" ] || { echo "benchmark incomplete" >&2; exit 1; }
for spec in business:jsonl statistics:jsonl traceroute:txt; do
    tool=${spec%%:*}; ext=${spec##*:}
    n=$(find "$OUT/run" -name "*-${tool}-*.${ext}" | wc -l)
    echo "  $tool.$ext: $n files"
    [ "$n" -eq "$EXPECTED" ] || { echo "$tool output incomplete" >&2; exit 1; }
done
echo "BENCHMARK PASS ($ACTUAL runs)"
