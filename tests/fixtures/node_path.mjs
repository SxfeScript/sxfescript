// Every path function, posix and win32, over a wide corpus, against what
// Node prints for the same corpus (node_path.expected, recorded from Node).
//
// posix has to match exactly -- it is what this runtime runs on. win32 is a
// port of the same algorithms and agrees with Node on the ordinary cases;
// where it does not, the difference is listed in node_path.known so it is
// visible rather than silent, and a NEW difference fails the test.
//
// To refresh after an intentional change:
//   node tests/fixtures/node_path.mjs > tests/fixtures/node_path.expected
import path from "node:path";
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };
const pieces = ["", ".", "..", "...", "a", "a.txt", ".hidden", "a..", "a.b.c", "C:", "C:\\", "C:x",
  "\\", "\\\\", "/", "//", "\\\\srv\\share", "\\\\srv\\share\\d", "/usr/local", "usr", "a/b", "a\\b",
  "a//b", "a\\\\b", "  ", "a b", "./x", "../y", "/a/../b", "C:\\a\\..\\b", "x/", "x\\"];
let out = 0;
for (const impl of ["posix", "win32"]) {
  const p = path[impl];
  for (const one of pieces) {
    console.log(impl, JSON.stringify(one),
      "n=" + JSON.stringify(p.normalize(one)),
      "d=" + JSON.stringify(p.dirname(one)),
      "b=" + JSON.stringify(p.basename(one)),
      "e=" + JSON.stringify(p.extname(one)),
      "a=" + p.isAbsolute(one));
    out++;
  }
  for (const a of pieces) for (const b of ["x", "..", "/y", "C:\\z", ""]) {
    console.log(impl, "join", JSON.stringify(a), JSON.stringify(b), "->", JSON.stringify(p.join(a, b)));
    out++;
  }
  // Absolute on both sides only, and absolute in the sense this half of the
  // module means: anything relative is resolved against the working
  // directory, and the answer would then depend on where this ran.
  const roots = impl === "posix"
    ? ["/a/b", "/a", "/", "/a/b/c", "/x/y"]
    : ["C:\\a\\b", "C:\\a", "C:\\", "\\\\srv\\share\\a", "\\\\srv\\share\\b", "D:\\a"];
  for (const a of roots) for (const b of roots) {
    console.log(impl, "relative", JSON.stringify(a), JSON.stringify(b), "->", JSON.stringify(p.relative(a, b)));
    out++;
  }
  console.log(impl, "sep", JSON.stringify(p.sep), "delimiter", JSON.stringify(p.delimiter));
}
console.log("cases:", out);

const here = (name) => new URL("./" + name, import.meta.url).pathname;
const expected = readFileSync(here("node_path.expected"), "utf8").trimEnd().split("\n");
const known = new Set(readFileSync(here("node_path.known"), "utf8").split("\n").filter(l => l && !l.startsWith("#")));

let bad = 0, allowed = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  if (known.has(expected[i])) { allowed++; continue; }
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0
  ? `node:path: ${printed.length - allowed} of ${printed.length} identical to Node, ${allowed} known win32 differences`
  : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
