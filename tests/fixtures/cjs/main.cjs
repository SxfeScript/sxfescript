// CommonJS: exports, module.exports replacement, JSON, caching, cycles, and
// the module-scoped identifiers.
const lib = require("./lib.cjs");
const factory = require("./replaced.cjs");
const data = require("./data.json");
const again = require("./lib.cjs");
const a = require("./cyclic-a.cjs");
console.log("exports:", lib.name, lib.bump(), lib.bump());
console.log("module.exports replaced:", factory(21));
console.log("json:", data.n);
console.log("cached same object:", again === lib, again.bump());
console.log("cycle:", a.name, a.sawB);
console.log("dirname is a dir:", typeof __dirname === "string" && __dirname.length > 0);
console.log("filename ends right:", __filename.endsWith("main.cjs"));
console.log("this is exports:", typeof module === "object" && typeof exports === "object");
let missing = "resolved";
try { require("./definitely-not-here.cjs"); } catch (e) { missing = e.constructor.name; }
console.log("missing throws:", missing);
