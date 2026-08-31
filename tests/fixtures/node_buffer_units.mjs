// latin1, Node's 7-bit "ascii" and utf16le in both directions, now native.
// The interesting inputs are the ones where a code unit is not a byte: a
// lone surrogate, an emoji's pair, anything above 0xff.
import { readFileSync } from "node:fs";
const lines = [];
const log = (...a) => { lines.push(a.join(" ")); };

const strings = ["", "hello", "héllo", "日本語", "🎉", "a\ud800b", "a\udc00b", "ÿĀ￿", "x".repeat(100)];
for (const s of strings)
  for (const enc of ["latin1", "ascii", "binary", "utf16le", "ucs2", "utf-16le"])
    log("enc", enc, JSON.stringify(s), Buffer.from(s, enc).toString("hex"),
        JSON.stringify(Buffer.from(s, enc).toString(enc)));
for (const hex of ["", "00", "41c1", "00d8", "ffff41", "e9", "010203"])
  for (const enc of ["latin1", "ascii", "binary", "utf16le", "ucs2"])
    log("dec", enc, hex, JSON.stringify(Buffer.from(hex, "hex").toString(enc)));

// Hex and base64 the way Node reads them: it stops at the first pair that
// is not hex, skips anything outside the base64 alphabet, and reads the
// string a byte at a time -- so a code unit above 0xff is truncated, not
// skipped, and an emoji's low half ends a base64 string.
for (const [enc, str] of [["hex", "4a4b\u00ff41"], ["hex", "zz"], ["hex", "4a\ud83c\udf89"],
                          ["hex", "4A4b"], ["hex", "4a4"], ["base64", "QUJD\u00ff"],
                          ["base64", "QU JD"], ["base64", "QUJD\ud83c\udf89QUJD"],
                          ["base64", "QUJ="], ["base64url", "-_8="], ["base64", "Q\nUJD"]])
  log("lenient", enc, JSON.stringify(str), Buffer.from(str, enc).toString("hex"));

const expected = readFileSync(new URL("./node_buffer_units.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(lines.length, expected.length); i++) {
  if (lines[i] === expected[i]) continue;
  bad++;
  console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  console.log("      got " + (lines[i] ?? "(nothing)"));
}
console.log(bad === 0 ? `Buffer code units: ${lines.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
