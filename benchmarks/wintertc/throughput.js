const { EventEmitter } = require('events');
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

// JSON: a 1MB API payload, parsed and written back out. This is the shape of
// a request body in any JSON API, and both directions are under test.
const doc = JSON.stringify(Array.from({ length: 2000 }, (_, i) => ({
  id: i,
  name: "record " + i,
  tag: "t" + (i % 7),
  bio: "the quick brown fox jumps over the lazy dog ".repeat(6),
  ok: i % 2 === 0,
  score: i * 3,
})));
t0 = performance.now(); total = 0;
for (let i = 0; i < 40; i++) total += JSON.stringify(JSON.parse(doc)).length;
console.log("json:", (performance.now() - t0).toFixed(1), "ms");
