// node:querystring, which is native C (js_qs_* in src/node.c). Every line
// below is printed by Node too, and node_querystring.expected holds what it
// printed, so this runs without Node and fails on any divergence.
//
// To refresh after an intentional change:
//   node tests/fixtures/node_querystring.mjs > tests/fixtures/node_querystring.expected
import qs from "node:querystring";
import { readFileSync } from "node:fs";

const lines = [];
const console = { log: (...args) => lines.push(args.join(" ")) };
const show = (v) => JSON.stringify(v, Object.keys(v ?? {}).sort());
const cases = [
  "a=1&b=2", "a=1&a=2&a=3", "a", "a=", "=b", "", "a=1&&b=2", "&&",
  "name=a+b", "city=S%C3%A3o+Paulo", "bad=%zz", "half=%", "p=%2Fslash",
  "x=1;y=2", "k%5B%5D=1&k%5B%5D=2", "utf=%F0%9F%8E%89", "eq=a=b",
  "sp=a%20b", "plus=a%2Bb", "empty=&next=1", "dup=1&dup=2&other=3",
];
for (const c of cases) console.log(JSON.stringify(c), "->", show(qs.parse(c)));
console.log("custom sep ->", show(qs.parse("a:1|b:2", "|", ":")));
console.log("maxKeys 2  ->", show(qs.parse("a=1&b=2&c=3", "&", "=", { maxKeys: 2 })));
const objs = [
  { a: 1, b: "two" }, { a: ["1", "2"] }, { "a b": "c d" }, { e: "" },
  { n: null, u: undefined, t: true, f: false, num: 4.5 }, { "é": "🎉" }, {},
];
for (const o of objs) console.log("stringify", JSON.stringify(o), "->", JSON.stringify(qs.stringify(o)));
console.log("stringify custom ->", JSON.stringify(qs.stringify({ a: 1, b: 2 }, "|", ":")));
console.log("escape   ->", qs.escape("a b/c?d=e&f+g'()!~*."));
console.log("unescape ->", qs.unescape("a%20b%2Fc%3Fd"));

const expected = readFileSync(new URL("./node_querystring.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(lines.length, expected.length); i++) {
  if (lines[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (lines[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `node:querystring: ${lines.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
