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

// Buffer.from of a view copies, and the copy must not share bytes with the
// original. A Float64Array is not raw bytes to Node: it truncates each
// element to a byte.
{
  const src = new Uint8Array([1, 2, 3]);
  const copy = Buffer.from(src);
  copy[0] = 9;
  log("from view copies", src[0], copy.toString("hex"), Buffer.isBuffer(copy));
  log("from a slice of a view", Buffer.from(new Uint8Array([1, 2, 3, 4]).subarray(1, 3)).toString("hex"));
  log("from a float array", Buffer.from(new Float64Array([1, 2])).toString("hex"));
  log("from an empty view", Buffer.from(new Uint8Array()).length);
  log("from an array", Buffer.from([65, 300, -1, 2.7]).toString("hex"));
  log("from an array-like", Buffer.from({ length: 3, 0: 1, 1: 2, 2: 3 }).toString("hex"));
}

// Buffer.concat, which is native now: with and without a length, a length
// that cuts the parts short, one that runs past them and leaves zeroes, and
// an empty list.
{
  const parts = [Buffer.from("abc"), Buffer.from("de"), Buffer.from("fghij")];
  log("concat", Buffer.concat(parts).toString());
  log("concat short", Buffer.concat(parts, 4).toString());
  log("concat long", Buffer.concat(parts, 20).length, Buffer.concat(parts, 20).toString("hex"));
  log("concat empty", Buffer.concat([]).length, Buffer.concat([], 3).toString("hex"));
  log("concat is a Buffer", Buffer.isBuffer(Buffer.concat(parts)));
  log("concat of views", Buffer.concat([new Uint8Array([1, 2]), new Uint8Array([3])]).toString("hex"));
}

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
