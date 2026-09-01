// process.cwd() is cached, so process.chdir() has to be the thing that
// clears it -- and a chdir that fails must not.
import path from "node:path";
import os from "node:os";
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + n + " got=" + got + (ok ? "" : " want=" + want)); };

const start = process.cwd();
check("absolute", path.isAbsolute(start), true);
check("stable", process.cwd(), start);

// The temporary directory may be a symlink, so what cwd reports after
// moving there is compared with itself rather than with the name used.
process.chdir(os.tmpdir());
const moved = process.cwd();
check("chdir is seen", moved !== start, true);
check("still seen twice", process.cwd(), moved);

let code = "";
try { process.chdir(path.join(moved, "no-such-directory-here")); } catch (e) { code = e.code; }
check("a failed chdir throws", code, "ENOENT");
check("a failed chdir changes nothing", process.cwd(), moved);

process.chdir(start);
check("back where we started", process.cwd(), start);
console.log(bad === 0 ? "process.cwd: cache follows chdir" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
