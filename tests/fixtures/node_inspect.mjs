// util.inspect, native now. What it prints is this runtime's own shape
// rather than Node's, so these are pinned here: every kind of value, the
// depth limit, a cycle, and the key quoting.
import util from "node:util";
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + n + " got=" + got + (ok ? "" : " want=" + want)); };

check("null", util.inspect(null), "null");
check("undefined", util.inspect(undefined), "undefined");
check("number", util.inspect(42), "42");
check("negative zero", util.inspect(-0), "0");
check("NaN", util.inspect(NaN), "NaN");
check("bigint", util.inspect(10n), "10n");
check("boolean", util.inspect(false), "false");
check("bare string", util.inspect("plain"), "plain");
check("string in a container", util.inspect({ s: 'a"b' }), '{ s: "a\\"b" }');
check("symbol", util.inspect(Symbol("s")), "Symbol(s)");
check("named function", util.inspect(function named() {}), "[Function: named]");
check("anonymous function", util.inspect(() => {}), "[Function: anonymous]");
check("date", util.inspect(new Date(0)), "1970-01-01T00:00:00.000Z");
check("invalid date", util.inspect(new Date(NaN)), "Invalid Date");
check("regexp", util.inspect(/ab+c/gi), "/ab+c/gi");
check("empty array", util.inspect([]), "[]");
check("array", util.inspect([1, 2, 3]), "[ 1, 2, 3 ]");
check("holes are undefined", util.inspect([1, , 3]), "[ 1, undefined, 3 ]");
check("empty object", util.inspect({}), "{}");
check("object", util.inspect({ a: 1, b: "x" }), '{ a: 1, b: "x" }');
check("keys that need quotes", util.inspect({ "b c": 1, $d: 2, _e: 3 }), '{ "b c": 1, $d: 2, _e: 3 }');
check("numeric keys come first", util.inspect({ b: 1, 2: 2 }), '{ "2": 2, b: 1 }');
check("empty map", util.inspect(new Map()), "Map(0) {}");
check("map", util.inspect(new Map([["k", 1]])), 'Map(1) { "k" => 1 }');
check("empty set", util.inspect(new Set()), "Set(0) {}");
check("set", util.inspect(new Set([1, "a"])), 'Set(2) { 1, "a" }');
check("typed array", util.inspect(new Uint8Array([1, 2, 3])), "Uint8Array(3) [ 1, 2, 3 ]");
check("float array", util.inspect(new Float64Array([1.5])), "Float64Array(1) [ 1.5 ]");
check("depth stops", util.inspect({ a: { b: { c: { d: 1 } } } }), "{ a: { b: { c: [Object] } } }");
check("deeper on request", util.inspect({ a: { b: { c: { d: 1 } } } }, { depth: 4 }), "{ a: { b: { c: { d: 1 } } } }");
check("array depth stops", util.inspect([1, [2, [3, [4]]]]), "[ 1, [ 2, [ 3, [Array] ] ] ]");
{
  const cycle = { name: "c" };
  cycle.self = cycle;
  check("cycle", util.inspect(cycle), '{ name: "c", self: [Circular *1] }');
  const shared = { x: 1 };
  check("shared is not a cycle", util.inspect({ l: shared, r: shared }), "{ l: { x: 1 }, r: { x: 1 } }");
}
check("error carries its stack", util.inspect(new Error("boom")).startsWith("Error: boom"), true);
check("isDeepStrictEqual still works", util.isDeepStrictEqual({ a: [1] }, { a: [1] }), true);
console.log(bad === 0 ? "util.inspect: shapes hold" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
