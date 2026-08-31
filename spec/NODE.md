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
encodings, lenient hex/base64 readers and numeric accessors, `util.format`,
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
