import crypto from "node:crypto";
import net from "node:net";
import EventEmitter from "node:events";

let bad = 0;
const check = (n, got, want) => { const ok = JSON.stringify(got) === JSON.stringify(want); if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + (ok ? "" : " want=" + JSON.stringify(want))); };

// Known vectors, so a wrong digest cannot pass by agreeing with itself.
check("sha256 hex", crypto.createHash("sha256").update("abc").digest("hex"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
check("md5 hex", crypto.createHash("md5").update("abc").digest("hex"),
      "900150983cd24fb0d6963f7d28e17f72");
check("sha1 base64", crypto.createHash("sha1").update("abc").digest("base64"),
      "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
check("update is chunked", crypto.createHash("sha256").update("a").update("bc").digest("hex"),
      crypto.createHash("sha256").update("abc").digest("hex"));

// RFC 4231 test case 1.
check("hmac sha256", crypto.createHmac("sha256", Buffer.alloc(20, 0x0b)).update("Hi There").digest("hex"),
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
// Key longer than the block size is hashed first; this exercises that branch.
check("hmac long key", crypto.createHmac("sha256", "k".repeat(100)).update("x").digest("hex").length, 64);

check("randomBytes length", crypto.randomBytes(16).length, 16);
check("randomBytes differ", crypto.randomBytes(16).equals(crypto.randomBytes(16)), false);
check("randomUUID shape", /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
      .test(crypto.randomUUID()), true);
check("timingSafeEqual same", crypto.timingSafeEqual(Buffer.from("ab"), Buffer.from("ab")), true);
check("timingSafeEqual differ", crypto.timingSafeEqual(Buffer.from("ab"), Buffer.from("ac")), false);
let threw = false;
try { crypto.timingSafeEqual(Buffer.from("a"), Buffer.from("ab")); } catch { threw = true; }
check("timingSafeEqual length mismatch throws", threw, true);

check("isIPv4", [net.isIPv4("10.0.0.43"), net.isIPv4("10.0.0.256"), net.isIPv4("::1")], [true, false, false]);
check("isIPv6", [net.isIPv6("::1"), net.isIPv6("fe80::1"), net.isIPv6("10.0.0.1")], [true, true, false]);
check("isIP zone index", net.isIP("fe80::1%eth0"), 6);
check("isIP", [net.isIP("10.0.0.1"), net.isIP("nope")], [4, 0]);

// Express copies EventEmitter.prototype onto a bare function, so the
// constructor never runs and `_events` has to appear on first use.
const mixin = function () {};
Object.assign(mixin, EventEmitter.prototype);
let heard = null;
mixin.on("ping", (v) => { heard = v; });
mixin.emit("ping", 7);
check("mixin without constructor", heard, 7);
check("EventEmitter.EventEmitter", EventEmitter.EventEmitter === EventEmitter, true);
check("EventEmitter.default", EventEmitter.default === EventEmitter, true);

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
