// A real Node-API addon, compiled by the build from tests/fixtures/napi/hello.c
// against Node's own headers. Every expectation here is Node's own output.
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);

let bad = 0;
const check = (n, got, want) => { const g = JSON.stringify(got);
  if (g !== JSON.stringify(want)) { bad++; console.log("FAIL " + n + " got=" + g + " want=" + JSON.stringify(want)); } };

const addonPath = process.env.SXN_TEST_ADDON;
if (!addonPath) { console.log("SKIP: SXN_TEST_ADDON not set"); process.exit(0); }
const a = require(addonPath);

check("string out", a.hello(), "hello from C");
check("numbers in and out", a.add(2, 3), 5);
check("string in and out", a.concat("hi"), "hi!");
check("objects and arrays", a.makeObject(), { answer: 42, squares: [0, 1, 4], label: "nested" });
check("calling back into JS", a.callBack((n) => n * 6), 42);
check("a thrown error crosses the boundary",
      (() => { try { a.thrower(); return "no throw"; }
               catch (e) { return [e.constructor.name, e.code, e.message]; } })(),
      ["RangeError", "ERR_RANGE", "out of range on purpose"]);
const buf = a.makeBuffer();
check("it can make a Buffer", [Buffer.isBuffer(buf), buf.toString()], [true, "abcd"]);
check("it can read a typed array", a.readTyped(new Uint8Array([1, 2, 3, 4])), 10);
check("wrap and unwrap native state", a.readCounter(a.makeCounter()), 100);
check("the module cache holds", require(addonPath) === a, true);
check("a missing addon is an error",
      (() => { try { require("/nonexistent/nope.node"); return "no throw"; }
               catch (e) { return "threw"; } })(), "threw");

// A real worker thread calling back into JS. The calls have to arrive on the
// loop thread, in order, and the process has to stay awake until they do.
const seen = [];
await new Promise((resolve) => {
  a.fromThread((n) => { seen.push(n); if (seen.length === 3) resolve(); });
});
check("a worker thread reaches JS", seen, [1, 2, 3]);

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
