// No "type" in the nearest package.json, so Node runs this as CommonJS.
module.exports = { ok: true };
if (typeof require.resolve !== "function") { console.log("FAIL require.resolve"); process.exit(1); }
if (require("node:module").prototype === undefined) { console.log("FAIL module.prototype"); process.exit(1); }
const near = require("./sibling.js");
if (near.n !== 7) { console.log("FAIL sibling " + JSON.stringify(near)); process.exit(1); }
console.log("cjs " + __filename.endsWith("plain.js") + " " + (typeof __dirname === "string"));
