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
| `fs`, `fs/promises` | File I/O, sync and promise-based. |
| `http` | `createServer`, `IncomingMessage`, `ServerResponse`, `ClientRequest`, `STATUS_CODES`, `METHODS`. The request body defers behind `_read` rather than pushing eagerly, because a body-parser attaches its listener after the handler returns — push first and it gets nothing. |
| `module` | The `Module` constructor (what `require('module').prototype` expects), `createRequire`, `builtinModules`, `isBuiltin`. |
| `net` | `isIP`/`isIPv4`/`isIPv6`, including IPv6 zone-index stripping (`fe80::1%eth0`). `Socket`/`Server` are not implemented and throw. |
| `os`, `path`, `querystring`, `url`, `util` | The usual surface — `inspect`, `format`, `promisify`, `deepEqual`, POSIX/Win32 path handling, and so on. |
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

## Encoding-name and Buffer performance

Two things specific to this layer are worth knowing if you're profiling
Buffer-heavy code: a literal encoding string (`"utf-8"`, `"hex"`) at a call
site is recognized by pointer identity against the atom table rather than by
hashing and comparing, and `Buffer.byteLength` computes the UTF-8 byte count
directly rather than encoding the string to measure it. Both are covered in
more depth, with numbers, in the README's benchmark section.
