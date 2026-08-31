# The Node-compatibility layer

This is what makes `sxn` usable as a Node alternative: CommonJS, the `node:`
builtins, and native-addon loading. It's the half of the runtime that a
mobile or embedded build can drop entirely without losing anything on the
`spec/RUNTIME.md` side — `spec/NATIVE.md` explains why that split exists for
the native-code case, and the same reasoning applies to this whole layer:
Node emulation is dead weight to an embedder with no Node surface of its own.

## Running a file the way Node runs it

`sxn` decides module-or-CommonJS the way Node does: `.mjs`/`.mts` are always
modules, `.cjs` is always CommonJS, and a plain `.js` file or an extensionless
one (every CLI an npm package ships) follows the nearest `package.json`'s
`"type"`, defaulting to CommonJS. A `#!/usr/bin/env node` shebang line is
stripped before evaluation, the way Node strips it, so those extensionless
CLIs run directly: `sxn ./node_modules/.bin/whatever` works.

A CommonJS module gets `require`, `module`, `exports`, `__filename`, and
`__dirname`, wrapped exactly the way Node wraps it. `require.resolve(spec)`
exists on every `require`. `require("node:module").createRequire(path)`
returns a `require` anchored at that path's directory rather than the caller's
— get this wrong and it resolves the wrong package's siblings.

## Module resolution

Bare specifiers resolve through `node_modules`, walking up from the importing
file — plain packages, scoped packages (`@scope/name`), and subpath imports.
A package's `exports` field is read for the `.` subpath, checking conditions
in the order `import`, `module`, `default`, `require`, `node`, then falling
back to `module`, then `main`. Circular `require` sees the same partially
filled `exports` a cycle sees in Node, rather than recursing forever.

## `.node` addons

`require("./thing.node")` and the `process.dlopen` it calls under the hood
both work, through a Node-API implementation built on QuickJS — full detail,
including what's implemented and what isn't, is `spec/NATIVE.md`. It's
listed here because it's the other reason this layer exists as a separate,
droppable piece: on iOS you can't `dlopen` code that arrived after the app
was signed, so this half of native-code support is inherently a
desktop-and-server capability, unlike `Sxn.ffi`.

## `node:` builtins

24 of the ~37 Node ships. What each one covers, briefly, and where it's
worth knowing the gap:

| Module | Covers |
|---|---|
| `assert`, `assert/strict` | The standard assertion functions. |
| `buffer` | See below — this one gets its own section. |
| `crypto` | `Hash`, `Hmac` (standard construction over the digest primitive), `randomBytes`, `randomUUID`, `timingSafeEqual`. |
| `events` | `EventEmitter`, including the mixin pattern (`Object.assign(fn, EventEmitter.prototype)`) Express uses, where `_events` is created lazily on first `on()`/`emit()` rather than in a constructor that never runs. |
| `fs`, `fs/promises` | `readFile`/`writeFile` and their sync forms, `existsSync`, `stat`/`lstat` and their sync forms with a real `Stats`, and `createReadStream` (which reads the file, rather than windowing a file too large to hold). |
| `http` | `createServer`, `IncomingMessage`, `ServerResponse`, `ClientRequest`, `STATUS_CODES`, `METHODS`. The request body defers behind `_read` rather than pushing eagerly, because a body-parser attaches its listener after the handler returns — push first and it gets nothing. |
| `module` | The `Module` constructor (what `require('module').prototype` expects), `createRequire`, `builtinModules`, `isBuiltin`. |
| `net` | `isIP`/`isIPv4`/`isIPv6`, including IPv6 zone-index stripping (`fe80::1%eth0`). `Socket`/`Server` are not implemented and throw. |
| `os` | Answered by libuv, not guessed: `hostname`, `cpus`, `totalmem`/`freemem`, `loadavg`, `uptime`, `networkInterfaces`, `availableParallelism`, `homedir`/`tmpdir`, `type`/`release`/`version`/`machine`, `userInfo`. |
| `path`, `querystring`, `url`, `util` | The usual surface — `inspect`, `format`, `promisify`, `deepEqual`, POSIX/Win32 path handling, and so on. |
| `perf_hooks` | Enough for timing code that reads `performance.now`-equivalent values. |
| `process` | `platform`, `arch`, `version`/`versions`, `stdout`/`stderr`/`stdin`, `hrtime`, `emitWarning`, `uptime`, `pid`, `env`, `argv`, `dlopen`. |
| `stream`, `stream/promises` | `Readable`/`Writable`/`Duplex`/`Transform`/`PassThrough`, `pipeline`, `finished`. The module export is the `Stream` function itself (some packages `require('stream')` and call it as a constructor), and `Readable` supports real `pipe`/`unpipe` — the latter matters because `finalhandler` calls it on every response, piped or not. |
| `string_decoder`, `timers`, `timers/promises`, `tty` | Small, focused shims. |
| `zlib` | `gzipSync`/`gunzipSync`/`deflateSync`/`inflateSync` and the stream equivalents (`createGzip` etc.), over the zlib already linked in. No `promises` namespace — Node doesn't have one either. |

Not implemented: `child_process`, `cluster`, `dns`, `http2`, `https`,
`readline`, `stream/web`, `tls`, `v8`, `vm`, `worker_threads`, `inspector`,
`async_hooks`. `child_process` is the one that stops `next build` today —
see `spec/NATIVE.md`'s account of running Next.js's own compiler for the
full trace of what does and doesn't stand in the way.

## Buffer

`Buffer` extends `Uint8Array` and matches Node's encoding behavior, verified
against Node's own output rather than against itself — a divergence in either
runtime fails the fixture:

- `utf8`/`utf-8`, `hex`, `base64`, `base64url`, `latin1`/`binary`, `ascii`,
  `ucs2`/`ucs-2`/`utf16le`/`utf-16le` — every encoding name, case-insensitive,
  in both directions.
- `hex` decoding stops at the first invalid pair rather than throwing, the
  way Node's own reader does (unlike the standard `Uint8Array.fromHex`, which
  throws).
- `base64` decoding skips characters it can't use, stops at `=`, accepts
  either alphabet, needs no padding, and reads one byte per UTF-16 code unit
  — which is why a multi-byte character truncates a base64 string early: its
  high surrogate half masks down to `=`.
- `Buffer.byteLength`, `compare`, `equals`, `concat`, `toJSON` (Node's
  `{type:"Buffer",data:[...]}` shape).
- The numeric accessors — `readUInt32BE`, `writeFloatLE`, `readBigInt64LE`
  and the rest of the forty, plus the variable-width `readUIntBE`/`writeIntLE`
  family — `write`, `copy`, `swap16`/`swap32`/`swap64`, `Buffer.compare` and
  `isEncoding`.
- `Buffer.from` copies when given a Buffer or a typed array, as Node does;
  it shares only when given an ArrayBuffer.

## Encoding-name and Buffer performance

Two things specific to this layer are worth knowing if you're profiling
Buffer-heavy code: a literal encoding string (`"utf-8"`, `"hex"`) at a call
site is recognized by pointer identity against the atom table rather than by
hashing and comparing, and `Buffer.byteLength` computes the UTF-8 byte count
directly rather than encoding the string to measure it. Both are covered in
more depth, with numbers, in the README's benchmark section.

## What is C and what is JavaScript

This layer started as one JavaScript file and has been moving into C a piece
at a time. What has gone over is what C is actually better at: byte and
string work with no JavaScript state of its own.

Native now: `path` in both halves, `querystring`, `net.isIP`, `os` in full
(from libuv), `fs`'s `stat`/`lstat` and the read primitives, `crypto`'s
digests, HMAC and `timingSafeEqual`, `zlib`'s deflate and inflate, Buffer's
encodings including latin1, Node's 7-bit `ascii` and utf16le in both
directions, lenient hex/base64 readers and numeric accessors, `util.format`,
`EventEmitter`'s `on`/`emit` fast path, and the structural comparison behind
`assert.deepStrictEqual`, `assert.deepEqual` and `util.isDeepStrictEqual`.

The last three moves are worth reporting honestly, because they say where the
seam is. The lenient base64 reader and `util.format` are correct and match
Node exactly, but neither is much faster than the JavaScript it replaced --
`format("a plain message")` went from 0.24 to 0.18 microseconds and the rest
is a wash. The deep comparison is slower: 4.58 microseconds per compare of a
small nested object against the JavaScript's 4.18, because every step of it
is a call back into the engine to read a property or compare two values, and
the interpreter does that for itself more cheaply than `JS_GetProperty` does
from outside. It is in C because this layer is being consolidated there and
it is the last piece of `node_compat.js` carrying real logic rather than
glue, and it is now checked against Node's own answers for 130 pairs, which
the JavaScript never was. Both facts belong in the same sentence. The work in
all three had already shrunk to the JavaScript-to-C boundary itself. That is
the shape of what is left everywhere else in this file.

The move after them was the opposite kind: `Buffer#toString("latin1")` and
its utf16le and `ascii` siblings were a `String.fromCharCode` appended in a
loop, which builds a whole new string per byte. Filling the code units in C
and handing the engine one string took 4KB of latin1 from 395 microseconds to
0.5. Nothing here is a boundary crossing per byte, which is why it moved so
far when the deep comparison did not move at all.

`require()` of a builtin moved for a third reason again: the specifier-to-
module table was an object literal rebuilt on every call, before the name was
even looked at. Static in C, a `require("node:path")` went from 2.30
microseconds to 0.18.

Inside `node:http`, the per-request header work moved too: lowercasing every
name and flattening it into `rawHeaders` was two JavaScript walks over the
same keys and is now one pass in C, 1.74 microseconds to 0.60 for a seven-
header request. That is 1.1 of the layer's 5.6 microseconds, taken from the
one part of it that is string work rather than object construction.

`node:crypto` had one of these left inside it: `update(data, "hex")` parsed
the string with `parseInt` on a two-character `substr` per byte, and base64
went through `atob`. Both now go through Buffer's native readers, which were
already there -- 4KB of hex input fell from 168 microseconds to 4.5 including
the digest itself.

`res.end()` joins what was written into one body, and that walked the chunk
list three times -- once to ask whether any of it was binary, once to convert,
once to join. In C the three shapes every real response actually is are
answered directly: one string went from 0.41 microseconds to 0.02, two byte
chunks from 0.74 to 0.17. A mix of strings and bytes is handed back to the
JavaScript, because concatenating strings is the engine's own job and C would
have to re-encode them to do it.

Object construction turned out to move as well, which the earlier note here
that C "would pay more at the boundary than it saves" got wrong for two
cases. `EventEmitter#once` allocated a closure that had to name itself in
order to remove itself; as a C function carrying the emitter, the name and
the listener, a once-and-emit went from 0.55 microseconds to 0.44. The socket
hung off every `node:http` request was eleven properties copied onto a fresh
emitter per request; the shape never varies, so the prototype is built once
and each request gets an object pointing at it -- 0.96 microseconds to 0.085,
another sixth of the layer's cost. The rule is not "objects stay in
JavaScript": it is that C wins wherever the work is repeated setup and loses
wherever it is a call back into the engine per step.

The same rule took two more closures out of every request. `IncomingMessage`
built one to defer pushing the body until something reads, and another to
mark itself complete on `end`; both are now shared native functions reading
their state off the request. Building the two closures cost 0.084
microseconds a request against 0.010 for the two fields that replaced them.

`res.setHeader` and its three siblings lowercase the name on every call, and
that is a scan over a short string rather than a call back into the engine
per step: 0.17 microseconds a call became 0.058.

`fs.statSync` had the same shape of waste: the native call built an object
with fifteen fields on it, and JavaScript then copied every one of them onto
a fresh `Stats` with a `for-in` loop and added four Dates. The native call
takes `Stats.prototype` now and fills the object once -- 3.30 microseconds a
stat became 2.75.

The same three-loop join -- total the lengths, allocate, copy each part in --
existed four times over: `Buffer.concat`, `node:crypto`'s digest input,
`node:zlib`'s stream flush and `res.end()`. There is one now, in C, one
memcpy per part: `Buffer.concat` of three 512-byte parts went from 0.77
microseconds to 0.36.

The two lenient readers finally lost their JavaScript halves. Node's hex and
base64 readers stop or skip rather than throwing, and they read a string a
byte at a time, so a code unit above 0xff is truncated -- which is why an
emoji ends a base64 string. The native readers used to hand such a string
back and let a JavaScript loop do it; they read the code units themselves
now, and the loops are gone.

A second was tried and thrown away for a better reason than speed. A stream
pushing a `Uint8Array` wraps it in a Buffer over the same bytes, which costs
0.24 microseconds a chunk; giving the array Buffer's prototype in place costs
0.11. But the array belongs to whoever pushed it, and changing its prototype
changes what their own object is. The faster answer was the wrong one.

One move was tried and thrown away, which is worth writing down because the
number is the only thing that settles it. A `Readable`'s constructor sets ten
own fields; done from C instead, the same ten stores cost 0.40 microseconds
against the interpreter's 0.34. `JS_SetPropertyStr` from outside is slower
than the interpreter's own store on a fresh object, so plain field
initialisation stays where it is. The socket above is not a
counter-example -- what made it fast was not setting the fields at all.

`Writable#write` built a closure per chunk for the callback it must hand
`_write`, closing over a `backpressure` flag that nothing ever set. It is a C
function carrying the stream and the caller's callback now. This one bought
almost nothing -- 0.195 microseconds a write became 0.190 -- and the first
version of it, a shared no-op for writes with no callback, was faster at
0.130 and wrong: it swallowed the error a failing `_write` reports, which
has to reach the stream's 'error' listeners whether anyone passed a callback
or not.

`StringDecoder` is native for utf-8 now -- it walks back at most three bytes
for a sequence that has not all arrived and keeps it for the next chunk,
where it used to run a `TextDecoder` with `{ stream: true }` per chunk: 0.330
microseconds to 0.120. The other encodings keep the `TextDecoder`. Node's own
answer for a stranded byte is one replacement character for the character
that never arrived, not one per byte, which its own output settled.

`node:util`'s `promisify`, `callbackify` and `inherits` were measured and
left alone: at 0.27, 0.14 and 0.55 microseconds they are already faster here
than in Node, which spends 1.37, 1.18 and 0.82 on the same three.

`process.nextTick` copied `arguments` into an array and built a closure over
it every time; it carries up to three arguments in the C closure now and
keeps the rest in an array, 0.620 microseconds to 0.143. A magic value of -1
for the array case is what found a sharp edge in the engine: `JS_NewCFunctionData`
stores its magic unsigned, so -1 came back as 65535 and the call read 65535
arguments off a four-slot array. It is 4 now.

A third sweep, over what a server actually touches, found two corruptions
rather than costs. `fs/promises.readFile` read the file as text and encoded
it back to bytes, so every byte that is not valid UTF-8 came back as the
replacement character -- a binary file was destroyed by reading it. It uses
the same native read the synchronous side does now, which is also three times
faster. `fs.writeFileSync` had the mirror of it: everything went through
`JS_ToCStringLen`, so a Buffer was written as its decimal digits. Bytes are
written as bytes now.

The second sweep, over `node:util`, `node:string_decoder` and `process`,
found something worse than any of the migrations: `process.cwd()` cost 7
microseconds against Node's 0.01. It was calling `getcwd()` every time, and
that walks the directory back to the root. Every relative path a package
resolves goes through it. It is cached now, and `process.chdir()` -- which
this runtime did not have at all -- is what clears the cache: 0.065
microseconds.

The sweep also turned up a bug rather than a cost. A `Readable` took chunks
off its queue with `shift()`, which copies the whole queue down by one every
time, so draining 20000 buffered chunks took 44 milliseconds against 1 for
2000 -- quadratic in the queue's length. Chunks leave through a cursor now:
the same 20000 take 3 milliseconds. This one is not a migration at all, and
no amount of C would have found it.

A sweep of what is left, measured rather than guessed, so the next person
does not have to re-derive it:

| Piece | Cost here | Where it goes |
| --- | --- | --- |
| `path.extname` | 0.060 us | already C |
| `Writable#write` | 0.130 us | C no-op callback; the rest is `_write` |
| `querystring.stringify` | 0.155 us | already C |
| `path.join` | 0.225 us | already C |
| `querystring.parse` | 0.235 us | already C |
| `Readable#push` + emit | 0.290 us | the emit is C; the wrap is not (see below) |
| `PassThrough#write` | 0.600 us | two emitter hops, both already C |
| `url.fileURLToPath` | 0.130 us | C, was 0.475 |
| `module.isBuiltin` | 0.045 us | C, was 0.415 |
| `process.nextTick` | 0.143 us | C, was 0.620 |
| `res.getHeaders` | 0.110 us | C, was 0.173 |
| `res.getHeaderNames` | 0.110 us | C, was 0.157 |
| `StringDecoder#write` | 0.120 us | C, was 0.330 |
| `res.writeHead` | 0.330 us | JS: it is setHeader in a loop, and that is C |
| `new Writable` | 0.360 us | JS: field stores, measured slower in C |
| `new Readable` | 0.500 us | JS: same |
| `readable.on("data")` | 1.16 us | JS: 0.5 of it is the deferred drain, which is the semantics |
| `new URL` | 0.885 us | the engine's own |
| `url.pathToFileURL` | 0.92 us | C text, was 1.30; `new URL` is 0.885 of what is left |
| `createRequire` | 1.00 us | already C |

Nothing in that list is a JavaScript loop any more. What remains in the file
around them is dispatch: argument shuffling, a check, and a call into
something that is already native.

Still JavaScript, with the reason measured rather than asserted:

- **`stream` and `http`.** A `node:http` request costs 13.4 us here against
  `Sxn.serve`'s 7.8 for the same reply, so the layer is 5.6 us of JavaScript
  -- worth attacking, if C could take it. It cannot: 0.9 us of that is
  constructing the Readable and the Writable, which are the API, not an
  implementation detail a handler cannot see; the rest is property writes and
  listener bookkeeping on those same objects, which C would perform through
  `JS_SetProperty` at more cost than the interpreter's own store. The gap is
  visible in the constructors themselves -- `new Readable` is 0.50 us here
  and 0.036 in Node -- and that is the no-JIT tradeoff this runtime has
  chosen, not something moving the file to C would change.
- **`util.inspect` and `assert.deepStrictEqual`** walk arbitrary JavaScript
  values. Every step would be a `JS_*` call; the C would be longer and no
  faster.
- **Buffer's lenient hex and base64 readers** are defined over UTF-16 code
  units — Node reads a string one code unit at a time and masks it, which is
  why an emoji ends a base64 string. A C function receives UTF-8 and cannot
  see that.
- **Thin wrappers** — `zlib`'s callback and promise forms, `fs`'s encoding
  branch, `process`, `os`'s method objects — are three lines each around a
  native call, and moving them would add C without removing work.
