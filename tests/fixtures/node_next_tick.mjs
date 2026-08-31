// process.nextTick, native now: it carries up to three arguments directly
// and keeps the rest in an array, so every arity has to arrive intact.
let bad = 0;
const check = (n, got, want) => { const ok = JSON.stringify(got) === JSON.stringify(want); if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + n + " got=" + JSON.stringify(got) + (ok ? "" : " want=" + JSON.stringify(want))); };

const seen = [];
process.nextTick(() => seen.push(["none"]));
process.nextTick((a) => seen.push([a]), 1);
process.nextTick((a, b) => seen.push([a, b]), 1, "two");
process.nextTick((a, b, c) => seen.push([a, b, c]), 1, "two", null);
process.nextTick((...a) => seen.push(a), 1, 2, 3, 4);
process.nextTick((...a) => seen.push(a), 1, 2, 3, 4, 5, 6, 7);
process.nextTick(function () { seen.push([this === undefined]); });

let code = "";
try { process.nextTick(42); } catch (e) { code = e.constructor.name; }
check("a non-function is refused", code, "TypeError");

await new Promise((r) => setTimeout(r, 10));
check("in order, with their arguments", seen, [
  ["none"], [1], [1, "two"], [1, "two", null], [1, 2, 3, 4], [1, 2, 3, 4, 5, 6, 7], [true],
]);
console.log(bad === 0 ? "process.nextTick: arguments and order hold" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
