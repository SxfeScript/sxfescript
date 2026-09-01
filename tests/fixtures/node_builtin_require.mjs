// require() of a builtin, now a native table lookup. Every specifier Node
// answers here, with and without the node: prefix, plus the two sub-modules
// and the one that must not resolve.
import module from "node:module";
const require = module.createRequire(import.meta.url);
let bad = 0;
const check = (name, got, want) => {
  const ok = got === want;
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + name + " got=" + got + " want=" + want);
};

const names = ["assert", "assert/strict", "buffer", "crypto", "events", "fs",
               "fs/promises", "http", "module", "net", "os", "path", "perf_hooks",
               "process", "querystring", "stream", "stream/promises",
               "string_decoder", "timers", "timers/promises", "tty", "url",
               "util", "zlib"];
for (const n of names) {
  for (const spec of [n, "node:" + n]) {
    const m = require(spec);
    check("require " + spec, m !== undefined && m !== null, true);
  }
}
check("buffer carries Buffer", require("buffer").Buffer === Buffer, true);
// The default export is this runtime's own convenience for `import buf from
// "node:buffer"`; Node has no such property on the CommonJS object.
check("buffer default", typeof require("node:buffer").default, typeof Buffer);
check("same object twice", require("fs") === require("node:fs"), true);
check("promises sub-module", typeof require("timers/promises").setTimeout, "function");

let code = "";
try { require("definitely-not-a-builtin"); } catch (e) { code = e.code; }
check("unknown module", code, "MODULE_NOT_FOUND");
check("isBuiltin known", module.isBuiltin("node:fs"), true);
check("isBuiltin unknown", module.isBuiltin("express"), false);

console.log(bad === 0 ? "node:module require: all builtins resolve" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
