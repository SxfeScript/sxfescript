// Every name in the Minimum Common API (min-common-api.proposal.wintertc.org)
// that does not live under WebAssembly, checked for presence and, where a
// check is cheap and meaningful, for behaviour. WebAssembly is listed at the
// end as the one part of the surface this runtime does not have.
let bad = 0;
const check = (name, ok, detail) => {
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + name + (detail === undefined ? "" : " " + detail));
};

const interfaces = [
  "AbortController", "AbortSignal", "Event", "EventTarget", "CustomEvent",
  "ErrorEvent", "MessageChannel", "MessageEvent", "MessagePort",
  "PromiseRejectionEvent", "DOMException", "Headers", "Request", "Response",
  "FormData", "Blob", "File", "CompressionStream", "DecompressionStream",
  "ByteLengthQueuingStrategy", "CountQueuingStrategy",
  "ReadableByteStreamController", "ReadableStream", "ReadableStreamBYOBReader",
  "ReadableStreamBYOBRequest", "ReadableStreamDefaultController",
  "ReadableStreamDefaultReader", "TransformStream",
  "TransformStreamDefaultController", "WritableStream",
  "WritableStreamDefaultController", "WritableStreamDefaultWriter",
  "TextDecoder", "TextDecoderStream", "TextEncoder", "TextEncoderStream",
  "URL", "URLSearchParams", "URLPattern", "Crypto", "CryptoKey", "SubtleCrypto",
  "Performance",
];
const globals = [
  "globalThis", "atob", "btoa", "clearTimeout", "clearInterval",
  "queueMicrotask", "reportError", "self", "setTimeout", "setInterval",
  "structuredClone", "fetch", "console", "crypto", "performance", "navigator",
];
for (const name of interfaces) check("interface " + name, typeof globalThis[name] === "function");
for (const name of globals) check("global " + name, typeof globalThis[name] !== "undefined");
for (const name of ["onerror", "onunhandledrejection", "onrejectionhandled"])
  check("handler " + name, name in globalThis);
check("navigator.userAgent", typeof navigator.userAgent === "string", navigator.userAgent);
check("total", interfaces.length + globals.length + 3 === 62, String(interfaces.length + globals.length + 3));

// Behaviour, not just names.
check("self is the global", self === globalThis);
check("performance is a Performance", performance instanceof Performance);
check("URLPattern segment", new URLPattern({ pathname: "/books/:id" }).exec("https://x.dev/books/7").pathname.groups.id === "7");
check("URLPattern stops at a slash", new URLPattern({ pathname: "/books/:id" }).test("https://x.dev/books/7/pages") === false);
check("URLPattern wildcard", new URLPattern("https://x.dev/a/*").test("https://x.dev/a/b/c"));
check("URLPattern optional group", new URLPattern({ pathname: "/opt{/:id}?" }).test("https://x.dev/opt"));
check("URLPattern rejects another host", new URLPattern("https://x.dev/a").test("https://y.dev/a") === false);

const ee = new ErrorEvent("error", { message: "m", filename: "f", lineno: 2 });
check("ErrorEvent carries its fields", ee.message === "m" && ee.filename === "f" && ee.lineno === 2);
const pre = new PromiseRejectionEvent("unhandledrejection", { reason: "r" });
check("PromiseRejectionEvent carries its reason", pre.reason === "r");

const text = "hello ".repeat(200);
const gz = await new Response(new Blob([text]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer();
check("CompressionStream shrinks", gz.byteLength < text.length, gz.byteLength + " < " + text.length);
const back = await new Response(new Blob([new Uint8Array(gz)]).stream().pipeThrough(new DecompressionStream("gzip"))).text();
check("DecompressionStream round trip", back === text);
for (const format of ["deflate", "deflate-raw"]) {
  const packed = await new Response(new Blob([text]).stream().pipeThrough(new CompressionStream(format))).arrayBuffer();
  const out = await new Response(new Blob([new Uint8Array(packed)]).stream().pipeThrough(new DecompressionStream(format))).text();
  check("round trip " + format, out === text);
}
let threw = false;
try { new CompressionStream("brotli"); } catch { threw = true; }
check("an unknown format throws", threw);

const bytes = new ReadableStream({ type: "bytes", start(c) { c.enqueue(new Uint8Array([1, 2, 3, 4, 5])); c.close(); } });
const reader = bytes.getReader({ mode: "byob" });
const first = await reader.read(new Uint8Array(2));
const second = await reader.read(new Uint8Array(8));
check("byob fills the view given", String(Array.from(first.value)) === "1,2");
check("byob keeps the rest", String(Array.from(second.value)) === "3,4,5");
check("byob ends", (await reader.read(new Uint8Array(4))).done === true);

const tsc = [];
await new ReadableStream({ start(c) { c.enqueue("a"); c.close(); } })
  .pipeThrough(new TransformStream({
    transform(chunk, controller) {
      check("transform controller has a class", controller instanceof TransformStreamDefaultController);
      controller.enqueue(chunk.toUpperCase());
    },
  }))
  .pipeTo(new WritableStream({ write(c) { tsc.push(c); } }));
check("transform ran", tsc.join("") === "A");

check("WebAssembly is the gap", typeof globalThis.WebAssembly === "undefined");
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
