// Same workload and iteration counts as throughput.sx / throughput.js.
// Buffer, TextEncoder and EventEmitter are the APIs under test, and Bun
// implements all three natively -- there is no Bun-specific alternative to
// substitute, so the only difference from throughput.js is ESM imports,
// which is how Bun code is normally written.
import { EventEmitter } from "node:events";
import { Buffer } from "node:buffer";
let N = 200000, t0 = performance.now(), total = 0;
for (let i = 0; i < N; i++) total += Buffer.from("hello world this is a test string", "utf-8").toString("hex").length;
console.log("buffer:", (performance.now() - t0).toFixed(1), "ms");
const enc = new TextEncoder();
const s = "hello world héllo 🙂 test string here";
t0 = performance.now(); total = 0;
for (let i = 0; i < N; i++) total += enc.encode(s).length;
console.log("textencoder:", (performance.now() - t0).toFixed(1), "ms");
N = 500000;
const ee = new EventEmitter();
let count = 0;
ee.on("x", (v) => { count += v; });
t0 = performance.now();
for (let i = 0; i < N; i++) ee.emit("x", i);
console.log("events:", (performance.now() - t0).toFixed(1), "ms");
