# The runtime surface

This is what `sxn` gives you independent of Node compatibility: the WinterCG
web APIs (45 of 55 names in the common surface), the `Sxn` host namespace, and
the engine capabilities that go with it. `spec/NODE.md` is the other half —
what runs because it imitates Node.
The split matters because only this half travels when the engine is embedded
elsewhere (`spec/NATIVE.md` explains why for the native-code case
specifically).

If you're choosing what to build against: code written to this surface plus
`spec/NODE.md`'s CommonJS/ESM loader runs on `sxn`, in a browser Worker, and
on Cloudflare Workers/Deno/Bun without a compatibility shim, because it's the
same surface those runtimes implement.

## Script kinds and entry points

`sxn file.sx` runs an SxfeScript file — ordinary JavaScript plus explicit
mutation, affine values, and borrow sigils, parsed natively with no separate
transform step (`spec/LANGUAGE.md`). `sxn file.ts` strips TypeScript types and
runs the result, also natively, with no build step. `sxn file.js` / `.mjs` /
`.cjs` run plain JavaScript. All four import each other freely: a `.sx`
module can `import` a `.ts` module and vice versa.

Module-or-script is decided the way Node decides it — see spec/NODE.md — with
one exception: `.sx` and `.ts` are always modules, because type stripping is
this project's own feature and has always meant ESM.

## `fetch`

A global `fetch(url, options)`, backed by libcurl, with methods, headers,
redirects, and streaming request and response bodies. `Sxn.fetch` is the same
function reachable through the host namespace, for code that wants to be
explicit about where it's calling.

- `Request`, `Response`, `Headers`, `URL`, `URLSearchParams` — the standard
  classes, including `Response.json/error/redirect`, `Request.clone`,
  `Headers.getSetCookie`, and the one exception the Fetch spec itself carves
  out: repeated `Headers.append("Set-Cookie", …)` calls stay separate instead
  of folding into one comma-joined value.
- A string body streams; `for await (const chunk of res.body)` and
  `res.body.pipeThrough(new TextDecoderStream())` both work on a response.
- `FormData`, `File`, `Blob` for multipart bodies.

## Sxn.serve — the HTTP server

```sx
const server = Sxn.serve({ port: 0 }, async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  if (url.pathname === "/echo") return Response.json(await req.json());
  return new Response("hi");
});
server.port;     // the port the OS chose, since `port: 0` asked it to pick
server.url;      // "http://127.0.0.1:PORT"
server.stop();
```

The handler receives a `Request` — `req.url` is absolute, so `new URL(req.url)`
gives you the path and query, and `req.text()`/`req.json()`/`req.arrayBuffer()`
read the body. It returns a `Response`, `Sxn.serve`'s own SSE helper, a
WebSocket upgrade, or a plain `{ statusCode, headers, body }` object, which is
the shape the native layer speaks and `node:http` is built on directly.
Response headers are emitted in declaration order; an array value repeats the
header (the multi-`Set-Cookie` case), and `Content-Length`/`Connection` are
filtered since those describe the framing rather than the payload the handler
wrote. `options` takes `port`, `hostname` (or `host`, Node's name for it; loopback
by default, `"0.0.0.0"` to accept from the network, an IPv6 literal or
`"localhost"` also work), and `reusePort`. With `reusePort: true` several
processes bind the same port and the kernel spreads connections across them,
which is how one program uses more than one core here -- there are no threads
and no cluster module. The kernels that distribute this way are Linux, the
BSDs, Solaris and AIX; on macOS the call fails with a message saying so
rather than quietly giving the last process every connection.

The returned handle reports `port`, `hostname`, `url`, and a `stop()` that
lets a process serve and then do something else, rather than block forever
the way a bare listener would; `stop()` closes the connections still open as
well as the listener. Connections are kept alive by default, as HTTP/1.1 requires,
and a request pipelined behind another is answered without waiting for a
further read. A request larger than 64MB is refused rather than buffered.

## Web Streams

`ReadableStream`, `WritableStream`, `TransformStream`, and both queuing
strategies, plus `TextEncoderStream`/`TextDecoderStream` built on them. A
fetch response body is a real `ReadableStream`, not a stand-in, so
`pipeThrough`, `pipeTo`, and `for await` all work on one. Not yet
implemented: `CompressionStream`/`DecompressionStream` (`node:zlib` covers
the same ground synchronously — see spec/NODE.md) and the BYOB reader.

## Crypto

`crypto.getRandomValues`, `crypto.randomUUID`, and `crypto.subtle` — the
Web Crypto surface, backed by OpenSSL. `node:crypto`'s `Hash`/`Hmac`/
`randomBytes`/`timingSafeEqual` cover the synchronous, Node-flavored version
of the same digests; see spec/NODE.md.

## Structured data and messaging

`structuredClone`, `MessageChannel`/`MessagePort`/`MessageEvent`,
`Event`/`EventTarget`/`CustomEvent`, `AbortController`/`AbortSignal`,
`DOMException`. `queueMicrotask`, `setTimeout`/`setInterval` and their
`clearX` counterparts, `performance.now` (bound directly to its C primitive,
not wrapped — see the README benchmarks for why that matters).

`console.log`/`info`/`debug` write to stdout and `console.error`/`warn` to
stderr, which is also what `process.stderr` is built on.

Not implemented: `URLPattern`, `BroadcastChannel`, `Worker`, `WebSocket` as an
*outbound client* (the server side — upgrading an incoming connection to a
WebSocket from a `Sxn.serve` handler — works), `ErrorEvent`,
`PromiseRejectionEvent`, and `Intl`.

## `Sxn.ffi` — calling a C function

```sx
const pow = Sxn.ffi("libSystem.B.dylib", "pow", ["f64", "f64"], "f64");
pow(2, 10);   // 1024
```

Backed by libffi and `dlopen`. Full type list, pointer/string handling, and
what's deliberately unsupported (structs by value, callbacks, variadics) are
in `spec/NATIVE.md`, along with why this is the half of native-code support
that belongs to the engine rather than to Node compatibility.

## Other `Sxn.*` entries

`Sxn.file(path)` and `Sxn.write(path, data)` for file I/O in the Bun-style
idiom; `Sxn.memoryUsage()`; `Sxn.version`.

## What's deliberately not here

Anything that only makes sense with a machine-code tier — a JIT, or
`process.dlopen`/`.node` addons — lives in the Node-compatibility layer
instead, not here, precisely so a build of this runtime that drops that layer
loses nothing on this side. See `spec/NATIVE.md` for the reasoning and
`spec/NODE.md` for what that layer covers.
