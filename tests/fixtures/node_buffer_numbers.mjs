// Buffer's numeric accessors and compare, which are native now
// (js_buffer_read / js_buffer_write_num / js_buffer_copy in src/node.c and
// sxn_bytes_compare in src/network.c). None of the read*/write* pair
// existed here before. The expected output is Node's.
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };

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
const cases = [["abc","abc"],["abc","abd"],["abd","abc"],["ab","abc"],["abc","ab"],["",""],["","a"],["a",""],["\xff","\x00"]];
for (const [a,b] of cases) {
  const x = Buffer.from(a, "binary"), y = Buffer.from(b, "binary");
  console.log(JSON.stringify(a), JSON.stringify(b), x.compare(y), x.equals(y), Buffer.compare(x, y));
}
console.log("sorted", [Buffer.from("b"), Buffer.from("a"), Buffer.from("ab")].sort(Buffer.compare).map(String).join(","));

const expected = readFileSync(new URL("./node_buffer_numbers.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `Buffer numbers: ${printed.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
