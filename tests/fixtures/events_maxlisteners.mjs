// Listeners added and never removed are the ordinary way a long-running
// server grows without bound, and the count crossing a limit is the standard
// signal. EventEmitter.defaultMaxListeners existed here as a constant nothing
// read; this asserts it now does something, once per emitter and event, and
// that a program which legitimately wants more can say so.
//
// The capture point is process.emitWarning, which is where both this runtime
// and Node route the warning, so this fixture's expectations can be checked
// against Node by running it there.

import { EventEmitter } from "node:events";

let failures = 0;
const check = (name, got, want) => {
  if (got !== want) {
    console.log("FAIL", name, "got", JSON.stringify(got), "want", JSON.stringify(want));
    failures += 1;
  }
};

const warnings = [];
const realEmitWarning = process.emitWarning;
process.emitWarning = (w) => {
  const name = w && w.name && w.name !== "Error" ? w.name + ": " : "";
  warnings.push(name + (w && w.message ? w.message : String(w)));
};
const since = () => warnings.length;
const added = (n) => warnings.slice(n);

check("default is 10", EventEmitter.defaultMaxListeners, 10);

// Ten is fine; the eleventh is what warns.
const a = new EventEmitter();
let mark = since();
for (let i = 0; i < 10; i++) a.on("x", () => {});
check("ten listeners are silent", added(mark).length, 0);

mark = since();
a.on("x", () => {});
const first = added(mark);
check("the eleventh warns", first.length, 1);
check("names the count", first[0].includes("11 x listeners added"), true);
check("names the limit", first[0].includes("MaxListeners is 10"), true);
check("is a MaxListenersExceededWarning", first[0].includes("MaxListenersExceededWarning"), true);

// Once per emitter and event, not once per registration.
mark = since();
for (let i = 0; i < 20; i++) a.on("x", () => {});
check("warns once, not per listener", added(mark).length, 0);

// A different event on the same emitter warns on its own.
mark = since();
for (let i = 0; i < 11; i++) a.on("y", () => {});
check("a second event warns separately", added(mark).length, 1);

// Nothing about the listener store changed.
check("listeners are all still there", a.listenerCount("x"), 31);
check("and still callable", (() => { let n = 0; a.on("z", () => n++); a.emit("z"); return n; })(), 1);

// Raising the limit suppresses it.
const b = new EventEmitter();
b.setMaxListeners(50);
check("getMaxListeners reflects it", b.getMaxListeners(), 50);
mark = since();
for (let i = 0; i < 40; i++) b.on("x", () => {});
check("under a raised limit is silent", added(mark).length, 0);
check("and the listeners are kept", b.listenerCount("x"), 40);

// Zero means unlimited, as in Node.
const c = new EventEmitter();
c.setMaxListeners(0);
mark = since();
for (let i = 0; i < 100; i++) c.on("x", () => {});
check("zero means unlimited", added(mark).length, 0);

// The default is a real accessor over the value the check uses.
EventEmitter.defaultMaxListeners = 2;
check("the default is settable", EventEmitter.defaultMaxListeners, 2);
const d = new EventEmitter();
mark = since();
d.on("q", () => {}); d.on("q", () => {}); d.on("q", () => {});
const lowered = added(mark);
check("a lowered default warns", lowered.length, 1);
check("at the lowered limit", lowered[0].includes("MaxListeners is 2"), true);
EventEmitter.defaultMaxListeners = 10;

// An emitter that never crosses the limit never allocates the warn marker.
const e = new EventEmitter();
e.on("x", () => {});
check("no marker until it warns", e._warnedEvents, undefined);

process.emitWarning = realEmitWarning;
if (failures !== 0) throw new Error(failures + " maxListeners checks failed");
console.log("maxListeners: all checks passed");
