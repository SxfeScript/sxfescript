# The runtime surface

This is what `sxn` gives you independent of Node compatibility: the WinterTC
web APIs (every name in the Minimum Common API except WebAssembly's), the
`Sxn` host namespace, and the engine capabilities that go with it. `spec/NODE.md` is the other half —
what runs because it imitates Node.
The split matters because only this half travels when the engine is embedded
elsewhere (`spec/NATIVE.md` explains why for the native-code case
specifically), and it is a build option rather than a description:
`cmake --preset minimal` builds this half with no `node:` layer and no libuv,
and the 29 fixtures of the `embed` test tier are the ones that have to pass
there. They run in the default build too, so a name on this page that starts
depending on the other half fails before anyone configures that build.

## What an embedder gets, and what it supplies

`sxn_install_runtime(ctx)` installs everything on this page onto a context
the host made. It lives in `libarcsx.a`, declared in
`include/sxn_runtime.h`, and the archive links nothing that knows what
Node is. Two things in it need something outside the engine to make
progress -- a timer has to fire later, and a promise waiting on I/O has to be
given a chance to settle -- and those go through `SxnLoopOps`
(`include/sxn_loop.h`): four functions, `timer_start`, `timer_stop`, `poll`
and an optional clock. Deliberately no sockets and no filesystem; a backend
that had to answer those would be a second libuv.

Two backends ship. The libuv one is the default and is what `sxn` has always
done. The other needs nothing beyond libc and libcurl, which waits on its own
sockets, so `fetch` and the timers both survive with no libuv linked and the
process still sleeps rather than spins. A host with a loop of its own --- a
frame pump, an application main loop -- supplies the third: it installs its
own `SxnLoopOps` and calls `sxn_runtime_tick(ctx, 0, slack_ns)` once a turn
instead of handing control to `sxn_run_event_loop`.

The node half is `libsxn.a`, a separate archive that links `arcsx`
and that nothing links back. With `SXN_ENABLE_NODE=OFF` it is not built,
which is why this page can promise a surface rather than describe one.

`Sxn.serve` and `Sxn.file` are the two things that do not survive that. A TCP
listener and a thread pool have no portable stand-in worth writing, so they
are capabilities of the libuv backend and are absent otherwise. Neither is a
WinterTC name, so the Minimum Common API is still answered in full; a build
without them simply has no `Sxn.serve` and no `Sxn.file`, and `bootstrap.js`
already checks for the primitive rather than assuming it. `Sxn.memoryUsage()`
keeps every field except `rss`, which needs a per-platform call that libuv is
where we keep.

If you're choosing what to build against: code written to this surface plus
`spec/NODE.md`'s CommonJS/ESM loader runs on `sxn`, in a browser Worker, and
on Cloudflare Workers/Deno/Bun without a compatibility shim, because it's the
same surface those runtimes implement.

## Script kinds and entry points

`sxn file.sx` runs an SxfeScript file — ordinary JavaScript plus explicit
mutation, affine values, and borrow sigils, parsed natively with no separate
transform step (`spec/LANGUAGE.md`). `sxn file.ts` parses TypeScript types
natively and runs the result, also with no build step; no code is emitted for
an annotation, but a declared scalar type is recorded rather than discarded
and spent on the bytecode that comes out (`spec/LANGUAGE.md`,
`spec/PERFORMANCE.md`). `sxn file.js` / `.mjs` / `.cjs` run plain JavaScript.
All four import each other freely: a `.sx` module can `import` a `.ts` module
and vice versa.

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

`ReadableStream`, `WritableStream`, `TransformStream`, both queuing
strategies, and each of the controller and reader classes the spec names, so
`instanceof` answers the way it does elsewhere. `TextEncoderStream`/
`TextDecoderStream` are built on them. A fetch response body is a real
`ReadableStream`, not a stand-in, so `pipeThrough`, `pipeTo`, and `for await`
all work on one.

A BYOB reader (`getReader({ mode: "byob" })`) fills the view you hand it and
keeps whatever did not fit for the next read. What it does not do is let the
*source* write into your buffer: `byobRequest` is always null, so a source
written to only ever fill a `byobRequest` finds nothing to fill. Reading is
still a copy, one chunk at a time; what BYOB buys here is the calling shape,
not zero-copy.

`CompressionStream`/`DecompressionStream` handle `gzip`, `deflate` and
`deflate-raw`, over the same zlib `node:zlib` uses, with one stream kept per
object so chunks compress as a single stream rather than one per chunk.

## URLPattern

`new URLPattern({ pathname: "/books/:id" })`, `test`, and `exec` with named
groups. Each URL component is compiled separately: `:name` captures up to the
component's own separator (`/` in a path, `.` in a hostname), `(...)` is a
regular expression written in place, `*` is anything, and `{...}?` makes what
it wraps optional. A component you leave out matches anything.

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

`ErrorEvent` and `PromiseRejectionEvent` exist, and so do the three handlers
that carry them.

## Errors that reach the top

The global object is an event target: `addEventListener("error", ...)` and
`onerror` both see an exception nothing caught, and either can keep it from
being printed — a listener by calling `preventDefault()`, `onerror` by
returning true. `reportError(e)` reports one without throwing it.

A promise rejection nothing handled is reported once every job that could
still have handled it has run, as an `unhandledrejection` event; if something
handles it later, `rejectionhandled` follows. With no handler registered,
nothing changes: an unhandled rejection is as quiet as it was before.

Not implemented: `BroadcastChannel`, `Worker`, `WebSocket` as an *outbound
client* (the server side — upgrading an incoming connection to a WebSocket
from a `Sxn.serve` handler — works), and `Intl`.

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

`Sxn.memoryUsage()` reports the engine allocator's accounting -- `mallocSize`,
`objects`, `gcCount` and the GC timings -- plus `rss`, which is what the
operating system says the process holds. The two move independently: the
system allocator may keep freed pages rather than return them, so `rss` can
stay high after a collection that reclaimed everything. Assert on
`mallocSize`; read `rss`.

`Sxn.gc()` collects reference cycles and returns the tracked size left behind.
Refcounting frees everything else the moment its last reference goes, but a
cycle -- an object that points at itself, a closure the object it captures
also holds -- needs the collector, and the collector otherwise runs only when
an allocation crosses a threshold. A process that stops allocating stops
collecting, which is why a server is also swept while its event loop is idle;
see `spec/CLI.md` for `--no-idle-gc` and `--idle-gc-floor`.

## What's deliberately not here

WebAssembly. It is the one part of the Minimum Common API this runtime does
not have, and it is not a small gap to close: QuickJS has no WebAssembly
engine, so `WebAssembly` is undefined rather than a stub that throws, which
lets feature detection do the right thing.

Anything that only makes sense with a machine-code tier — a JIT, or
`process.dlopen`/`.node` addons — lives in the Node-compatibility layer
instead, not here, precisely so a build of this runtime that drops that layer
loses nothing on this side. See `spec/NATIVE.md` for the reasoning and
`spec/NODE.md` for what that layer covers.
