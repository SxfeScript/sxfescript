// Bare specifier resolution: plain name, scoped name, subpath, and an
// "exports" map whose conditions nest (the shape tinybench ships).
import { hi } from "plain";
import { scoped } from "@scope/pkg";
import { root } from "subby";
import { deep } from "subby/lib/deep.js";
import { via } from "exp";
import { local } from "./sibling.mjs";
const L = [];
L.push("plain=" + hi());
L.push("scoped=" + scoped());
L.push("bare root=" + root);
L.push("subpath=" + deep);
L.push("nested exports=" + via);
L.push("relative still works=" + local);
let missing = "resolved";
try { await import("definitely-not-installed"); } catch (e) { missing = e.constructor.name; }
L.push("unresolved throws=" + missing);
console.log(L.join("\n"));
