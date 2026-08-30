#!/bin/sh
# Side-by-side sxn vs node vs bun. No category is hidden. Each runtime runs the
# same workload with the same iteration counts, written in that runtime's
# idiomatic form (Bun.serve/Bun.env for bun, Sxn.serve for sxn); Buffer,
# TextEncoder and EventEmitter are the APIs under test and are the same in all
# three.
#
# Every runtime is overridable, because a non-interactive shell does not have
# the PATH a login shell does and a runtime installed under ~/.local or ~/.bun
# would otherwise be silently skipped:
#
#   SXN=build/release/sxn NODE=node BUN=~/.bun/bin/bun sh benchmarks/wintercg/run.sh
#
# bun stays optional and its rows are skipped with a note when it is absent.
set -e
DIR="$(dirname "$0")"
SXN="${SXN:-$DIR/../../build/release/sxn}"
NODE="${NODE:-node}"
RUNS="${RUNS:-1000}"

# Wall-clock timing goes through the checked-in python timer rather than the
# shell's `time`, which dash -- /bin/sh on Debian and Ubuntu -- does not have.
TIMER="$DIR/timeone.py"
PY="${PYTHON:-python3}"

resolve() { command -v "$1" 2>/dev/null || { [ -x "$1" ] && printf '%s\n' "$1"; }; }

SXN_BIN="$(resolve "$SXN" || true)"
NODE_BIN="$(resolve "$NODE" || true)"
BUN_BIN="$(resolve "${BUN:-bun}" || true)"
[ -n "$SXN_BIN" ]  || { echo "sxn not found at '$SXN' (set SXN=path)"; exit 1; }
[ -n "$NODE_BIN" ] || { echo "node not found at '$NODE' (set NODE=path)"; exit 1; }
if [ -n "$BUN_BIN" ]; then HAVE_BUN=1; else HAVE_BUN=0; fi

echo "== runtimes under test =="
printf 'sxn   %s (%s)\n'  "$SXN_BIN"  "$("$SXN_BIN" --version 2>/dev/null || echo '?')"
printf 'node  %s (%s)\n'  "$NODE_BIN" "$("$NODE_BIN" --version 2>/dev/null || echo '?')"
if [ "$HAVE_BUN" = 1 ]; then
  printf 'bun   %s (%s)\n' "$BUN_BIN" "$("$BUN_BIN" --version 2>/dev/null || echo '?')"
else
  printf 'bun   not found -- skipping its rows (set BUN=path to include it)\n'
fi
echo

# One timed launch, in milliseconds.
timeone() { "$PY" "$TIMER" "$@"; }

echo "== real-world end-to-end task (wall clock, as actually invoked) =="
"$SXN_BIN" "$DIR/server.sx" & SRV=$!
# Stop the server even if a timed command fails or the script is interrupted.
trap 'kill $SRV 2>/dev/null || true' EXIT INT TERM
sleep 1
printf -- "-- sxn --  ";  timeone "$SXN_BIN"  "$DIR/realworld.sx"
printf -- "-- node -- ";  timeone "$NODE_BIN" "$DIR/realworld.js"
[ "$HAVE_BUN" = 1 ] && { printf -- "-- bun --  "; timeone "$BUN_BIN" "$DIR/realworld.bun.js"; }
kill $SRV 2>/dev/null || true
trap - EXIT INT TERM
echo
echo "== cold start =="
printf -- "-- sxn --  ";  timeone "$SXN_BIN"  "$DIR/coldstart.sx"
printf -- "-- node -- ";  timeone "$NODE_BIN" "$DIR/coldstart.js"
[ "$HAVE_BUN" = 1 ] && { printf -- "-- bun --  "; timeone "$BUN_BIN" "$DIR/coldstart.bun.js"; }
echo
echo "(these two rows are one launch each; startup20.py reports medians over 20)"
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
measure_throughput sxn "$DIR/throughput.sx" "$SXN_BIN"
measure_throughput node "$DIR/throughput.js" "$NODE_BIN"
[ "$HAVE_BUN" = 1 ] && measure_throughput bun "$DIR/throughput.bun.js" "$BUN_BIN"
echo
echo "== pause consistency =="
echo "-- sxn --";  "$SXN_BIN"  "$DIR/pause.sx"
echo "-- node --"; "$NODE_BIN" "$DIR/pause.js"
[ "$HAVE_BUN" = 1 ] && { echo "-- bun --"; "$BUN_BIN" "$DIR/pause.bun.js"; }
