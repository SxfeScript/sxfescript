# SxfeScript and SXN

SXN is a standalone QuickJS-based runtime for `.sx` systems code and ordinary
JavaScript. SxfeScript adds explicit mutation, affine values, borrows, and
erasable TypeScript-style annotations without a Vite or AOT build step.

This repository is intentionally independent from Rayact. Its QuickJS source
was a direct snapshot of Rayact's customized fork at commit `66f4965`, and has
since diverged under its own name, **ArcSX** (see
`third_party/QUICKJS-PROVENANCE.md` for the full lineage).

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the example:

```sh
./build/debug/sxn examples/velocity.sx
```

## Current implementation status

The repository contains a working QuickJS-backed CLI, an in-memory `.sx`
frontend, fixed-layout arena primitives, package workflow commands, an LSP
transport, VS Code language packaging, specifications, and tests. The native
opcode lowering, full control-flow ownership pass, native npm registry backend,
and semantic LSP features are tracked in `spec/IMPLEMENTATION.md` and are not
yet represented as complete production implementations.

The runtime also includes native HTTP serving with SSE/WebSocket response modes
and a libcurl-backed global `fetch`; see `spec/NETWORK.md`. Applications use the
Bun-style `Sxn` namespace (`Sxn.serve`, `Sxn.file`, `Sxn.write`, and
`Sxn.fetch`). The `qjs:*` modules remain internal compatibility modules.

## Benchmarks: sxn vs Node vs Bun

`benchmarks/wintercg/run.sh` runs matched WinterCG-style workloads against
`sxn`, Node and Bun side by side, across four categories. No category is
hidden -- the others win the ones you'd expect them to. Each runtime runs the
same workload with the same iteration counts, written in that runtime's
idiomatic form (`Bun.serve`/`Bun.env` for Bun, `Sxn.serve` for sxn); Buffer,
TextEncoder and EventEmitter are the APIs under test and are the same in all
three. Bun is optional -- its rows are skipped with a note if it isn't
installed.

```sh
sh benchmarks/wintercg/run.sh
```

Minimum of 5+ runs, macOS 26.6.2 (arm64), Node v25.2.1, Bun 1.2.17.
Startup rows are the average of 20 process launches:

| Category | sxn | Node | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task (wall clock, as invoked) | **9 ms** | 88 ms | 15 ms | sxn |
| Cold start | **9 ms** | 45 ms | 9 ms | sxn / Bun tie |
| Sustained throughput: TextEncoder | 19.4 ms | 38.7 ms | **6.0 ms** | Bun |
| Sustained throughput: Buffer ops | 31.0 ms | **23.6 ms** | 26.8 ms | Node |
| Sustained throughput: EventEmitter | 20.3 ms | **4.9 ms** | 9.0 ms | Node |
| Pause consistency: worst single pause | **0.03 ms** | 0.18 ms | 2.60 ms | sxn |
| Pause consistency: total time | 390 ms | **242 ms** | 280 ms | Node |

sxn wins the categories dominated by process startup and one-shot work,
where there is no JIT to warm up, and has by far the tightest worst-case
pause. It beats Node on TextEncoder throughput but not Bun, and trails both
on the remaining hot loops: JavaScriptCore and V8 are JITs, QuickJS is an
interpreter by design, and that gap is not closable by tuning around it.

The throughput rows reflect a series of ArcSX/runtime optimizations (all
tagged `arcsx:` in `third_party/quickjs`), roughly in order of payoff:

- **Arena allocator**, ported from upstream quickjs-ng. Small objects come
  from per-size arenas instead of individual `malloc`s, and the refcount/GC
  header moved into the allocation block header. Each `Buffer.from(...)`
  pass allocated 7-8 blocks; recycling them is what took Buffer 83->36 ms
  and TextEncoder 65->23.5 ms in a single change, and cut the pause
  benchmark's total time from 1.1 s to 0.41 s.
- **Pinned core-type shapes.** QuickJS interns the empty shape behind
  `new Foo()` in a runtime-wide table, but nothing holds a reference to it,
  so a loop that allocates and drops one object per iteration destroys the
  shape with the last object and rebuilds it on the next -- and every shape
  free also flushes the property cache below. Keeping one throwaway Buffer,
  Uint8Array and ArrayBuffer alive per context pins those shapes; worth ~14%
  of the Buffer loop on its own.
- **Typed-array property fast path**: property names that provably can't be
  numeric indices (`.toString`, `.toHex`) stay on the interpreter's inline
  lookup path instead of bailing to the generic exotic-object path.
- **Polymorphic inline caches** for property reads. Each entry belongs to
  one call site -- keyed by the bytecode address of the read's atom operand,
  which pins the atom, so only the receiver's shape is compared -- and
  remembers up to four shapes it has seen, each with the prototype depth and
  slot index of the holder. A repeated read costs a shape compare and
  pointer derefs instead of a hash probe per prototype level. Worth ~20% on
  deep prototype chains (idiomatic class code) and ~5% on the loops above.
  Multi-way is what makes it safe to use: a single-way, per-site cache
  measured 12% *slower* than no per-site keying at all on a four-shape call
  site, because the site thrashed one slot. Only the *location* is cached,
  never a value, and the generation stamp is bumped at every point that can
  move a property, so a stale entry can't be read.
- **One-pass UTF-8 encoding** straight into the final buffer
  (`JS_NewUint8ArrayFromString` / `JS_NewArrayBufferFromString`), a native
  `Buffer.from(str, "utf-8")` that skips the JS subclass-constructor round
  trip (`JS_NewUint8ArrayWithProto`), `TextEncoder.prototype.encode` bound
  directly to its C primitive, and atom-identity event-type lookup plus
  direct fast-array listener access in `EventEmitter` (`JS_GetFastArray`).
- **A memo for `emit()`'s listener resolution**, valid only while the
  `_events` object, the event-name string object and a listener-mutation
  counter all still match. It holds strong references to what it keys on, so
  a cached object cannot be freed and have its address reused underneath the
  entry -- which is what made pointer identity safe here rather than a bet.
  Worth 12% of the events loop.

Cumulatively: Buffer 102->31 ms, TextEncoder 76->19.4 ms, EventEmitter
37->20.3 ms, with zero GC cycles during the loops throughout.

What's left in the EventEmitter gap is the interpreted-bytecode floor
itself: ~11.6 ms of that 20.3 ms is just invoking the listener closure 500k
times, which no amount of work outside the interpreter can remove. Closing
that means giving ArcSX a JIT -- its own multi-month project, not a
benchmark tuning pass.
