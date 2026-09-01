// Buffer's numeric accessors, write, copy, compare and the swaps -- all
// native (js_buffer_* in src/node.c, sxn_bytes_compare in src/network.c).
// Most of these did not exist here before. The expected output is Node's.
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };

{
const b = Buffer.alloc(16);
b.writeUInt8(0xff, 0); b.writeInt8(-2, 1);
b.writeUInt16LE(0x1234, 2); b.writeUInt16BE(0x1234, 4);
b.writeInt32LE(-123456, 6); b.writeFloatBE(1.5, 10);
console.log("hex", b.toString("hex"));
console.log("readUInt8", b.readUInt8(0), "readInt8", b.readInt8(1));
console.log("u16le", b.readUInt16LE(2), "u16be", b.readUInt16BE(4));
console.log("i32le", b.readInt32LE(6), "floatBE", b.readFloatBE(10));
const d = Buffer.alloc(8); d.writeDoubleLE(Math.PI, 0);
console.log("double", d.readDoubleLE(0), d.toString("hex"));
const big = Buffer.alloc(8); big.writeBigUInt64BE(12345678901234567890n, 0);
console.log("bigu64", String(big.readBigUInt64BE(0)), big.toString("hex"));
const sig = Buffer.alloc(8); sig.writeBigInt64LE(-42n, 0);
console.log("bigi64", String(sig.readBigInt64LE(0)));
const src = Buffer.from("hello world"), dst = Buffer.alloc(5);
console.log("copied", src.copy(dst, 0, 6, 11), dst.toString());
try { b.readUInt32BE(14); } catch (e) { console.log("range ->", e.constructor.name); }
console.log("compare", Buffer.compare(Buffer.from("a"), Buffer.from("b")), Buffer.isEncoding("hex"), Buffer.isEncoding("nope"));
}

{
const cases = [["abc","abc"],["abc","abd"],["abd","abc"],["ab","abc"],["abc","ab"],["",""],["","a"],["a",""],["\xff","\x00"]];
for (const [a,b] of cases) {
  const x = Buffer.from(a, "binary"), y = Buffer.from(b, "binary");
  console.log(JSON.stringify(a), JSON.stringify(b), x.compare(y), x.equals(y), Buffer.compare(x, y));
}
console.log("sorted", [Buffer.from("b"), Buffer.from("a"), Buffer.from("ab")].sort(Buffer.compare).map(String).join(","));
}

{
const b = Buffer.alloc(12);
b.writeUIntBE(0x123456, 0, 3); b.writeUIntLE(0x123456, 3, 3);
console.log("hex", b.toString("hex"));
console.log("uintBE", b.readUIntBE(0, 3), "uintLE", b.readUIntLE(3, 3));
const s = Buffer.alloc(6); s.writeIntBE(-1000, 0, 3); s.writeIntLE(-1000, 3, 3);
console.log("intBE", s.readIntBE(0, 3), "intLE", s.readIntLE(3, 3), s.toString("hex"));
for (const w of [1,2,3,4,5,6]) { const t = Buffer.alloc(6); t.writeUIntBE(255, 0, w); console.log("w"+w, t.readUIntBE(0, w), t.toString("hex")); }
const sw = Buffer.from([1,2,3,4,5,6,7,8]);
console.log("swap16", Buffer.from(sw).swap16().toString("hex"));
console.log("swap32", Buffer.from(sw).swap32().toString("hex"));
console.log("swap64", Buffer.from(sw).swap64().toString("hex"));
try { Buffer.from([1,2,3]).swap16(); } catch (e) { console.log("odd swap ->", e.constructor.name); }
try { b.readUIntBE(0, 7); } catch (e) { console.log("width 7 ->", e.constructor.name); }
}

{
const b = Buffer.alloc(10, 0x2e);
console.log("wrote", b.write("hello"), b.toString());
const c = Buffer.alloc(10, 0x2e);
console.log("offset", c.write("hi", 3), c.toString());
const d = Buffer.alloc(4, 0x2e);
console.log("truncated", d.write("hello"), d.toString());
const e = Buffer.alloc(10, 0x2e);
console.log("length cap", e.write("hello", 0, 3), e.toString());
const f = Buffer.alloc(3, 0x2e);
console.log("multibyte", f.write("héllo"), JSON.stringify(f.toString()));
const g = Buffer.alloc(8, 0x2e);
console.log("encoding arg", g.write("hey", "utf8"), g.toString());
}

{
const src = Buffer.from([1,2,3]);
const copy = Buffer.from(src);
copy[0] = 99;
console.log("src after writing to the copy:", src[0], "copy:", copy[0]);
const u8 = new Uint8Array([1,2,3]);
const fromU8 = Buffer.from(u8);
fromU8[0] = 42;
console.log("uint8array after:", u8[0], "buffer:", fromU8[0]);
const ab = new ArrayBuffer(3);
const view = Buffer.from(ab);
view[0] = 7;
console.log("arraybuffer view shares:", new Uint8Array(ab)[0]);
}

const expected = readFileSync(new URL("./node_buffer_numbers.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `Buffer: ${printed.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
