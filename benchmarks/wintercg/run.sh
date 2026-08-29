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
echo "-- sxn --";  "$SXN" "$DIR/throughput.sx"
echo "-- node --"; node "$DIR/throughput.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; bun "$DIR/throughput.bun.js"; }
echo
echo "== pause consistency =="
echo "-- sxn --";  "$SXN" "$DIR/pause.sx"
echo "-- node --"; node "$DIR/pause.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; bun "$DIR/pause.bun.js"; }
