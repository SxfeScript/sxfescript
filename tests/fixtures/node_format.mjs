// util.format, which is C now (js_util_format in src/node.c) apart from the
// two cases that need util.inspect. The expected output is Node's.
import util from "node:util";
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };
const f = util.format;
console.log(JSON.stringify(f("hello")));
console.log(JSON.stringify(f("%s world", "hello")));
console.log(JSON.stringify(f("%d + %d = %d", 1, 2, 3)));
console.log(JSON.stringify(f("%i", 4.9)));
console.log(JSON.stringify(f("%f", 4.5)));
console.log(JSON.stringify(f("%j", { a: 1 })));
console.log(JSON.stringify(f("%s", { a: 1 })));
console.log(JSON.stringify(f("%o", { a: { b: { c: 1 } } })));
console.log(JSON.stringify(f("%%")));
console.log(JSON.stringify(f("%% %s", "x")));
console.log(JSON.stringify(f("%z", 1)));
console.log(JSON.stringify(f("%s and %s", "one")));
console.log(JSON.stringify(f("a", "b", "c")));
console.log(JSON.stringify(f("count: %d", "12abc")));
console.log(JSON.stringify(f("%s", null), f("%s", undefined)));
console.log(JSON.stringify(f("%d", 10n)));
console.log(JSON.stringify(f("%c red", "color: red")));
console.log(JSON.stringify(f(1, 2)));
console.log(JSON.stringify(f("trailing", { x: 1 })));

const expected = readFileSync(new URL("./node_format.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `util.format: ${printed.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
