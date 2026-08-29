const path = require('path');
const { EventEmitter } = require('events');
const bus = new EventEmitter();
let sum = 0;
bus.on("row", (v) => { sum += v; });
async function main() {
  const r = await fetch(process.env.BENCH_URL || "http://127.0.0.1:8996/api");
  const data = JSON.parse(await r.text());
  for (const v of data.items) bus.emit("row", v);
  const enc = new TextEncoder().encode(data.name);
  const hex = Buffer.from(enc.buffer).toString("hex");
  const u = new URL("https://example.com/a/b?x=1");
  console.log(sum, hex, path.basename(u.pathname), u.searchParams.get("x"));
}
main();
