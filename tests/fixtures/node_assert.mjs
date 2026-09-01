// node:assert's structural comparison, which is C now. Every case is one
// question Node already has an answer for, so the answers are diffed rather
// than asserted here.
import assert from "node:assert";
import util from "node:util";
import { readFileSync } from "node:fs";

// Output is collected and matched against what Node printed, line for line.
const lines = [];
const console = { log: (...a) => { lines.push(a.join(" ")); } };


const show = (name, fn) => {
  let out;
  try { fn(); out = "ok"; } catch (e) { out = e.name === "AssertionError" ? "throws" : "error:" + e.name; }
  console.log(name + " -> " + out);
};
const deep = (name, a, b) => {
  show("strict  " + name, () => assert.deepStrictEqual(a, b));
  show("loose   " + name, () => assert.deepEqual(a, b));
  console.log("isDeep  " + name + " -> " + util.isDeepStrictEqual(a, b));
};

deep("numbers", 1, 1);
deep("number vs string", 1, "1");
deep("zero signs", 0, -0);
deep("NaN", NaN, NaN);
deep("null vs undefined", null, undefined);
deep("empty objects", {}, {});
deep("flat objects", { a: 1, b: "x" }, { a: 1, b: "x" });
deep("key order", { a: 1, b: 2 }, { b: 2, a: 1 });
deep("extra key", { a: 1 }, { a: 1, b: 2 });
deep("nested", { a: { b: [1, 2, { c: 3 }] } }, { a: { b: [1, 2, { c: 3 }] } });
deep("nested differs", { a: { b: [1, 2, { c: 3 }] } }, { a: { b: [1, 2, { c: 4 }] } });
deep("array vs object", [1, 2], { 0: 1, 1: 2 });
deep("array holes", [1, , 3], [1, undefined, 3]);
deep("dates", new Date(1000), new Date(1000));
deep("dates differ", new Date(1000), new Date(1001));
deep("invalid dates", new Date(NaN), new Date(NaN));
deep("regexps", /ab+/gi, /ab+/gi);
deep("regexps differ", /ab+/g, /ab+/i);
deep("typed arrays", new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 3]));
deep("typed arrays differ", new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 4]));
deep("typed array kinds", new Uint8Array([1]), new Int8Array([1]));
deep("maps", new Map([["a", 1]]), new Map([["a", 1]]));
deep("maps differ", new Map([["a", 1]]), new Map([["a", 2]]));
deep("maps nested values", new Map([["a", { x: 1 }]]), new Map([["a", { x: 1 }]]));
deep("sets", new Set([1, 2]), new Set([2, 1]));
deep("sets differ", new Set([1, 2]), new Set([1, 3]));
deep("prototypes", Object.create(null), {});
class A { constructor() { this.x = 1; } }
class B { constructor() { this.x = 1; } }
deep("classes", new A(), new A());
deep("different classes", new A(), new B());
deep("errors", new Error("x"), new Error("x"));
deep("symbol keys ignored", { [Symbol("s")]: 1 }, {});
deep("non-enumerable ignored", Object.defineProperty({ a: 1 }, "h", { value: 2 }), { a: 1 });
{
  const a = { name: "a" }; a.self = a;
  const b = { name: "a" }; b.self = b;
  deep("cycles", a, b);
  const c = { name: "c" }; c.self = c;
  deep("cycles differ", a, c);
}
{
  const shared = { x: 1 };
  deep("shared subtrees", { l: shared, r: shared }, { l: { x: 1 }, r: { x: 1 } });
}
deep("strings", "abc", "abc");
deep("boxed vs primitive", new String("a"), "a");
deep("boxed", new Number(1), new Number(1));
deep("booleans", true, 1);
deep("functions", function f() {}, function f() {});

show("ok true", () => assert.ok(1));
show("ok false", () => assert.ok(0));
show("equal loose", () => assert.equal(1, "1"));
show("strictEqual", () => assert.strictEqual(1, "1"));
show("notStrictEqual", () => assert.notStrictEqual(1, 2));
show("notDeepStrictEqual", () => assert.notDeepStrictEqual({ a: 1 }, { a: 2 }));
show("throws catches", () => assert.throws(() => { throw new Error("x"); }));
show("throws misses", () => assert.throws(() => {}));
show("match", () => assert.match("hello", /ell/));
show("match fails", () => assert.match("hello", /zzz/));

const expected = readFileSync(new URL("./node_assert.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(lines.length, expected.length); i++) {
  if (lines[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (lines[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `node:assert: ${lines.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
