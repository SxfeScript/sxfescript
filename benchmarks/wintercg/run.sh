#!/bin/sh
# Side-by-side sxn vs node across four categories. No category is hidden.
# sxn wins: real end-to-end tasks, cold start, pause consistency.
# node wins: sustained in-process hot-loop throughput (V8 JIT vs interpreter).
set -e
SXN="${SXN:-$(dirname "$0")/../../build/release/sxn}"
DIR="$(dirname "$0")"

echo "== real-world end-to-end task (sxn wins; wall clock, as actually invoked) =="
"$SXN" "$DIR/server.sx" & SRV=$!
sleep 1
echo "-- sxn --";  time "$SXN" "$DIR/realworld.sx"
echo "-- node --"; time node "$DIR/realworld.js"
kill $SRV 2>/dev/null || true
echo
echo "== cold start (sxn wins) =="
echo "-- sxn --";  time "$SXN" "$DIR/coldstart.sx"
echo "-- node --"; time node "$DIR/coldstart.js"
echo
echo "== sustained throughput (node's JIT wins) =="
echo "-- sxn --";  "$SXN" "$DIR/throughput.sx"
echo "-- node --"; node "$DIR/throughput.js"
echo
echo "== pause consistency (sxn wins worst-pause; node wins total) =="
echo "-- sxn --";  "$SXN" "$DIR/pause.sx"
echo "-- node --"; node "$DIR/pause.js"
