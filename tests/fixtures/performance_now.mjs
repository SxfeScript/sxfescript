// performance.now is the C primitive bound directly, with no JS wrapper.
// These pin the shape user code can observe, so the binding cannot regress
// into something with a different name, arity, or this-sensitivity.
let bad = 0;
const check = (name, got, want) => {
  const ok = got === want;
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + name + " got=" + got + " want=" + want);
};
check("typeof", typeof performance.now, "function");
check("name", performance.now.name, "now");
check("length", performance.now.length, 0);
check("returns number", typeof performance.now(), "number");
check("finite", Number.isFinite(performance.now()), true);

// Monotonic and advancing: the clock must never run backwards.
let prev = performance.now(), mono = true;
for (let i = 0; i < 5000; i++) { const t = performance.now(); if (t < prev) mono = false; prev = t; }
check("monotonic", mono, true);
const a = performance.now();
let s = 0; for (let i = 0; i < 500000; i++) s += i;
check("advances", performance.now() > a, true);

// Measured from a time origin at startup, not the epoch.
check("origin is startup", performance.now() < 600000, true);

// Sub-millisecond resolution: an integer-only clock would fail this.
let frac = false;
for (let i = 0; i < 200 && !frac; i++) if (performance.now() % 1 !== 0) frac = true;
check("sub-ms resolution", frac, true);

// this-independent: unlike Node, which brand-checks Performance, this
// runtime has always allowed a detached reference. Pinned deliberately.
const detached = performance.now;
check("detached call", typeof detached(), "number");
check("call(null)", typeof performance.now.call(null), "number");
check("extra args ignored", typeof performance.now(1, 2, 3), "number");

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
