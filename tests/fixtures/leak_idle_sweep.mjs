// The cycle sweep must fire on its own when the event loop goes quiet.
//
// leak_cycle_stress.sx covers Sxn.gc(), which any program can call. This
// covers the case nobody calls anything: a daemon takes a burst, the burst
// leaves cyclic garbage, and then it waits for the next request. Without the
// sweep that garbage is held forever, because QuickJS collects only from an
// allocation and a waiting process makes none -- and the collection at the
// burst's peak has already raised the threshold above what was leaked.
//
// Run with --no-idle-gc this fixture is expected to report NOT-SWEPT; ctest
// runs it both ways.

let failures = 0;
const check = (name, ok, detail) => {
  if (!ok) { console.log("FAIL", name, detail); failures += 1; }
};

const sweepDisabled = process.argv.includes("--expect-no-sweep");

function burst(n) {
  const batch = [];
  for (let i = 0; i < n; i++) {
    const s = { i, body: new Array(24).fill(i) };
    s.self = s;
    s.cb = () => s.body;
    batch.push(s);
  }
  return batch.length;
}

const before = Sxn.memoryUsage().mallocSize;
burst(200000);
const peak = Sxn.memoryUsage();
check("burst crossed the 32MB floor", peak.mallocSize > before + 32 * 1024 * 1024,
      "peak " + peak.mallocSize);

// Idle. Each tick blocks the loop for longer than the sweep's threshold, so
// the loop can tell it is waiting rather than working. Nothing here calls
// Sxn.gc().
let ticks = 0;
await new Promise((done) => {
  const id = setInterval(() => {
    if (++ticks >= 4) { clearInterval(id); done(); }
  }, 120);
});

const after = Sxn.memoryUsage();
const swept = after.mallocSize < before + 4 * 1024 * 1024;

if (sweepDisabled) {
  check("--no-idle-gc holds the garbage", !swept,
        "malloc " + after.mallocSize + " gc " + after.gcCount);
  console.log(failures === 0 ? "idle sweep: NOT-SWEPT as expected" : "idle sweep: unexpected sweep");
} else {
  check("the loop swept while idle", swept,
        "malloc " + after.mallocSize + " vs before " + before);
  check("a collection is what did it", after.gcCount > peak.gcCount,
        "gc " + peak.gcCount + " -> " + after.gcCount);
  console.log("idle sweep: reclaimed " + (peak.mallocSize - after.mallocSize) + " bytes with no explicit call");
}

if (failures !== 0) throw new Error(failures + " idle-sweep checks failed");
