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
