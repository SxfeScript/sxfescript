#!/bin/sh
# Side-by-side sxn vs node vs bun across four categories. No category is
# hidden. Each runtime runs the same workload with the same iteration counts,
# written in that runtime's idiomatic form (Bun.serve/Bun.env for bun,
# Sxn.serve for sxn); Buffer, TextEncoder and EventEmitter are the APIs under
# test and are the same in all three.
# bun is optional: skipped with a note if it is not installed.
set -e
SXN="${SXN:-$(dirname "$0")/../../build/release/sxn}"
DIR="$(dirname "$0")"
RUNS="${RUNS:-1000}"
if command -v bun >/dev/null 2>&1; then HAVE_BUN=1; else HAVE_BUN=0; echo "(bun not installed -- skipping its rows)"; fi

echo "== real-world end-to-end task (wall clock, as actually invoked) =="
"$SXN" "$DIR/server.sx" & SRV=$!
sleep 1
echo "-- sxn --";  time "$SXN" "$DIR/realworld.sx"
echo "-- node --"; time node "$DIR/realworld.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; time bun "$DIR/realworld.bun.js"; }
kill $SRV 2>/dev/null || true
echo
echo "== cold start =="
echo "-- sxn --";  time "$SXN" "$DIR/coldstart.sx"
echo "-- node --"; time node "$DIR/coldstart.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; time bun "$DIR/coldstart.bun.js"; }
echo
echo "== sustained throughput =="
echo "(runs: $RUNS; set RUNS=N to override; 1000 is intended for stable aggregate samples)"
median() {
  awk '{v[NR]=$1} END { if (NR % 2) print v[(NR + 1) / 2]; else print (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}
measure_throughput() {
  label="$1"; script="$2"; shift 2
  tmp="$(mktemp -t sxn-throughput.XXXXXX)"
  i=0
  while [ "$i" -lt "$RUNS" ]; do "$@" "$script" >>"$tmp"; i=$((i + 1)); done
  printf -- "-- %s (median) --\n" "$label"
  for metric in buffer textencoder events; do
    value="$(awk -v metric="$metric" '$1 == metric ":" { print $2 }' "$tmp" | sort -n | median)"
    printf "%s: %s ms\n" "$metric" "$value"
  done
  rm -f "$tmp"
}
measure_throughput sxn "$DIR/throughput.sx" "$SXN"
measure_throughput node "$DIR/throughput.js" node
[ "$HAVE_BUN" = 1 ] && measure_throughput bun "$DIR/throughput.bun.js" bun
echo
echo "== pause consistency =="
echo "-- sxn --";  "$SXN" "$DIR/pause.sx"
echo "-- node --"; node "$DIR/pause.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; bun "$DIR/pause.bun.js"; }
