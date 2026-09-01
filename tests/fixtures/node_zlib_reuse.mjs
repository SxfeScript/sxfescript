// The compressor and the decompressor are kept between calls and reset
// rather than rebuilt, so a run of calls has to keep giving the same bytes
// -- including when the window or the level changes between them, which is
// what makes the kept stream unusable and forces a fresh one.
import zlib from "node:zlib";
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + n + " got=" + got + (ok ? "" : " want=" + want)); };

const text = "hello world ".repeat(20);
const bytes = Buffer.from(text);

// Same settings, over and over: the second call is the one using a reset
// stream rather than a fresh one.
const first = zlib.gzipSync(bytes).toString("hex");
for (let i = 0; i < 5; i++) check("gzip is stable " + i, zlib.gzipSync(bytes).toString("hex"), first);
for (let i = 0; i < 5; i++) check("round trip " + i, zlib.gunzipSync(zlib.gzipSync(bytes)).toString(), text);

// Different containers around the same deflate data: each changes the window
// bits, so the kept stream cannot be reused for the next one.
check("deflate then gzip", zlib.gunzipSync(zlib.gzipSync(bytes)).toString(), text);
check("gzip then deflate", zlib.inflateSync(zlib.deflateSync(bytes)).toString(), text);
check("deflate then raw", zlib.inflateRawSync(zlib.deflateRawSync(bytes)).toString(), text);
check("raw then zlib", zlib.inflateSync(zlib.deflateSync(bytes)).toString(), text);

// Levels, alternating: a kept stream carries its level, so a different one
// has to be noticed.
const cheap = zlib.gzipSync(bytes, { level: 1 }).toString("hex");
const dear = zlib.gzipSync(bytes, { level: 9 }).toString("hex");
check("levels differ", cheap === dear, false);
for (let i = 0; i < 3; i++) {
  check("level 1 stable " + i, zlib.gzipSync(bytes, { level: 1 }).toString("hex"), cheap);
  check("level 9 stable " + i, zlib.gzipSync(bytes, { level: 9 }).toString("hex"), dear);
}
check("level 1 round trip", zlib.gunzipSync(zlib.gzipSync(bytes, { level: 1 })).toString(), text);

// Sizes either side of the output buffer's first guess, and empty input.
for (const size of [0, 1, 1023, 1024, 5000, 100000]) {
  const payload = Buffer.from("a".repeat(size));
  check("size " + size, zlib.gunzipSync(zlib.gzipSync(payload)).length, size);
}
// A failed inflate must not leave a broken stream behind for the next call.
let threw = false;
try { zlib.gunzipSync(Buffer.from("not gzip data at all")); } catch { threw = true; }
check("bad input throws", threw, true);
check("and the next call still works", zlib.gunzipSync(zlib.gzipSync(bytes)).toString(), text);

console.log(bad === 0 ? "node:zlib: reset streams give the same bytes" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
