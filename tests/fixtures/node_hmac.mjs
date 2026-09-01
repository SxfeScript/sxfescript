// HMAC and timingSafeEqual, which are OpenSSL's now (sxn_hmac and
// sxn_timing_safe_equal in src/network.c). The expected digests are Node's.
import crypto from "node:crypto";
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };
for (const algo of ["sha1", "sha256", "sha512", "md5"]) {
  for (const [key, data] of [["k", "message"], ["", "empty key"], ["a".repeat(200), "long key"], ["k", ""]]) {
    const h = crypto.createHmac(algo, key).update(data).digest("hex");
    console.log(algo, JSON.stringify(key.slice(0, 12)), JSON.stringify(data), h);
  }
}
const streamed = crypto.createHmac("sha256", "k").update("a").update("b").update("c").digest("base64");
console.log("streamed", streamed);
console.log("equal", crypto.timingSafeEqual(Buffer.from("abc"), Buffer.from("abc")));
console.log("differ", crypto.timingSafeEqual(Buffer.from("abc"), Buffer.from("abd")));
try { crypto.timingSafeEqual(Buffer.from("ab"), Buffer.from("abc")); } catch (e) { console.log("mismatch ->", e.constructor.name); }

// Input encodings: update() reads hex, base64 and latin1 through Buffer's
// native readers now, and every one of them has to hash the same bytes.
for (const [text, enc] of [["deadbeef", "hex"], ["DEADBEEF", "hex"], ["QUJD", "base64"],
                           ["QUJD", "base64url"], ["\u00ff\u00fe", "latin1"], ["abc", "utf8"], ["abc", undefined]]) {
  console.log("hash", enc, JSON.stringify(text), crypto.createHash("sha256").update(text, enc).digest("hex"));
  console.log("hmac", enc, JSON.stringify(text), crypto.createHmac("sha256", "key").update(text, enc).digest("base64"));
}

const expected = readFileSync(new URL("./node_hmac.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `node:crypto hmac: ${printed.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
