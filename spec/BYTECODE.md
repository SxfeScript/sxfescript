# Precompiled bytecode: `.sxbc`

`sxn` can skip parsing a file entirely and run its already-compiled bytecode
instead. This exists for two different reasons that happen to share one
mechanism:

- **Distribution.** `sxn compile app.sx` produces `app.sxbc`; ship that
  instead of the source and there is nothing left to parse on the machine
  that finally runs it. `--strip` drops the compiling machine's own file
  paths from the output, for when the source shouldn't be reconstructible
  from a stack trace.
- **Startup, on a large file.** `sxn --compile-cache app.sx` compiles once,
  caches the result next to the source, and reuses it on every later launch
  until the source changes. This is the same idea already applied to this
  runtime's own bootstrap (see the README's benchmark section), turned into
  something a user's own script can opt into.

Both produce and consume the same file format, so `sxn app.sxbc` runs either
one's output directly.

## Is it worth it for your script?

Measured on this runtime's own hardware (Apple M4, Release build), median of
9 runs, whole-process wall clock including startup:

| Script | From source | From bytecode | Saved |
|---|---:|---:|---:|
| `console.log("hi")` | 7.8 ms | 6.9 ms | 12% |
| 32k-line generated file (618 KB) | 15.2 ms | 9.6 ms | 37% |

Skipping the parse always saves something, because there is always a parse to
skip — but it scales with how much there is to parse. A one-line script gets
a small, real win from skipping the tokenizer and AST setup entirely. A
large generated file, a bundled app, or a big TypeScript-emitted script gets
a large one. `--compile-cache` is the flag to reach for once a script is big
enough, or launched often enough, that the difference shows up in something
you're measuring; for a small script run once, it's not going to move
anything you'd notice.

## `sxn compile <file> [-o out.sxbc] [--strip]`

```sh
sxn compile app.sx              # writes app.sxbc next to it
sxn compile app.sx -o dist/app.sxbc
sxn compile app.sx --strip -o dist/app.sxbc   # no local paths in the output
```

Works on anything `sxn` can run as an entry point: `.sx`, `.ts`, `.js`,
`.mjs`, `.cjs`, module or CommonJS, decided the same way running it directly
would decide (`spec/NODE.md`). The output name defaults to the input's name
with its extension replaced by `.sxbc`.

`--strip` removes line-number and local-variable debug tables (so a stripped
error reports a bytecode offset, not a source line) and, separately, embeds
the source's bare filename instead of its full path at compile time, so
nothing about the machine or directory the source lived in survives into the
shipped file. Verify what you're about to ship with `strings out.sxbc` if
that matters to you.

## `sxn --compile-cache <file> [args...]`

Runs `file` exactly as `sxn file` would, except: before running, it checks
for an `.sxbc` cache next to the source. If the cache is missing or older
than the source (by mtime), it compiles fresh and writes the cache; either
way, execution then runs from bytecode. A script invoked repeatedly parses
once, not on every launch — the common case for a CLI tool people run
often, or a dev server that restarts on every save without its own source
having changed on most of those restarts.

The cache is invisible to the script itself: `process.argv` and `__filename`
still show the original source path, not the internal `.sxbc` file.

## `sxn app.sxbc`

Runs a `.sxbc` file directly, as if it were the source it was compiled from.
`require()` and `import` inside it resolve normally, against the directory
the `.sxbc` file itself sits in.

## Format and limits

A `.sxbc` file is 5 bytes of header — a magic number this runtime checks
before trusting the rest as bytecode, so a corrupt or foreign file fails with
a clear message rather than a confusing one from deep inside the engine —
followed by QuickJS's own serialized bytecode for either a compiled module or
a compiled CommonJS wrapper function. The format is tied to this runtime's
exact build (the same `BC_VERSION` dependency the lazily-loaded builtins
have, `spec/IMPLEMENTATION.md`): a `.sxbc` compiled by one version of `sxn`
is not guaranteed to load in another, and a version mismatch is reported
rather than misread.

**Only compile trusted code.** `JS_ReadObject` with bytecode enabled is, by
QuickJS's own documentation, not a safe format to parse untrusted input —
unlike source text, a crafted bytecode blob can misdirect the interpreter
directly. Compile your own code, or code you already trust as source; don't
treat a `.sxbc` from an untrusted party as safer to run than the
`.js`/`.mjs`/`.cjs` it might have come from.

A `.sxbc`'s dependencies (whatever it `import`s or `require`s) still resolve
and load as ordinary source at run time — compiling one file does not pull
its dependency tree into the same blob. Compiling a whole app ahead of time
currently means compiling each of its own files individually; there is no
bundler step here.
