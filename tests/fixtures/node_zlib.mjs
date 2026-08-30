import zlib from "node:zlib";
import { gzipSync, gunzipSync, createGzip, createGunzip } from "node:zlib";
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const text = "hello world ".repeat(40);

p("gzip round trip", gunzipSync(gzipSync(text)).toString() === text);
p("deflate round trip", zlib.inflateSync(zlib.deflateSync(text)).toString() === text);
p("raw round trip", zlib.inflateRawSync(zlib.deflateRawSync(text)).toString() === text);
p("gzip magic", Array.from(gzipSync("x")).slice(0, 2));
p("compresses", gzipSync(text).length < text.length);
p("empty input", gunzipSync(gzipSync("")).length);
p("binary safe", (() => { const b = new Uint8Array([0, 255, 0, 128, 10, 13]);
  const out = gunzipSync(gzipSync(b)); return Array.from(out); })());
p("unzip accepts gzip", zlib.unzipSync(gzipSync(text)).toString() === text);
p("unzip accepts zlib", zlib.unzipSync(zlib.deflateSync(text)).toString() === text);
p("returns Buffer", Buffer.isBuffer(gzipSync("x")));

// callback form
p("callback", await new Promise((res) => zlib.gzip(text, (e, out) =>
  res(e ? "err" : gunzipSync(out).toString() === text))));
p("callback error", await new Promise((res) =>
  zlib.gunzip(new Uint8Array([1,2,3,4]), (e) => res(e ? "errored" : "no"))));
// Node promisifies the callback forms rather than shipping a promises
// namespace, so this is the supported route.
const gzipAsync = (await import("node:util")).promisify(zlib.gzip);
p("promisified", await gzipAsync(text).then((b) => gunzipSync(b).toString() === text));

// streams
p("createGzip/createGunzip", await (async () => {
  const gz = createGzip();
  const chunks = [];
  gz.on("data", (c) => chunks.push(c));
  const done = new Promise((r) => gz.on("end", r));
  gz.write("part one "); gz.write("part two"); gz.end();
  await done;
  return gunzipSync(Buffer.concat(chunks)).toString();
})());
console.log(L.join("\n"));
