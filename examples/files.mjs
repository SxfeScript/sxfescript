// Reading and writing files. Run it: sxn examples/files.mjs
//
// Sxn.file(path) and Sxn.write(path, data) are the runtime's own file I/O.
// node:fs works too, for code that already expects it.

import { tmpdir } from "node:os";
import { join } from "node:path";
import { readFile } from "node:fs/promises";

const path = join(tmpdir(), "sxn-example.txt");

await Sxn.write(path, "written by the example\n");

const file = Sxn.file(path);
console.log(JSON.stringify(await file.text()));

// The same file through the Node surface.
console.log("via node:fs ->", JSON.stringify(await readFile(path, "utf8")));

console.log("sxn version:", Sxn.version);
