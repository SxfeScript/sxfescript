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

**Run the tests against a Debug build.** QuickJS gates its leak tracking on
`#ifndef NDEBUG` (`ENABLE_DUMPS` in `third_party/quickjs/quickjs.c`), so in a
Release build the `sxn-leak-check` test still runs but has nothing to detect
and always passes. A Debug build is what actually catches a leaked atom,
object or string -- an atom leak in the `node:*` layer sat unnoticed behind a
green Release run until it aborted the first Debug one.

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
| Sustained throughput: Buffer ops | 25.9 ms | **24.0 ms** | 27.4 ms | Node, sxn 2nd |
| Sustained throughput: TextEncoder | 18.3 ms | 39.5 ms | **6.2 ms** | Bun |
| Sustained throughput: EventEmitter | 20.7 ms | **5.0 ms** | 9.2 ms | Node |
| Pause consistency: worst single pause | **0.05 ms** | 0.20 ms | 2.62 ms | sxn |

That pause row measures allocations that die immediately, which is the
pattern most favourable to refcounting. On a workload that keeps objects
live (`benchmarks/workload/pause_survivors.js`: 2000 survivors while
churning 2M allocations) the worst pause is 0.099 ms here against Node's
0.203 and Bun's 0.241 -- still ahead, but by 2-3x rather than 50x. Quote the
smaller number.
| Pause consistency: total time | 381 ms | **245 ms** | 281 ms | Node |
| Parse 32k-line generated file | **0.01 s** | 0.05 s | 0.02 s | sxn |

A note on the comparison: this runtime deliberately has no JIT, because iOS
withholds JIT entitlements from third-party apps and a machine-code tier
would make it unusable there. The rows below where a JIT runtime pulls ahead
are therefore measuring against a technique this project cannot adopt, not a
gap awaiting optimization -- see `spec/IMPLEMENTATION.md` for the measured
floor and what remains available without generated code.

The parse row is new: parsing was quadratic in declarations per scope until
the resolver's linear scans were indexed, and a 32k-line generated file now
parses faster here than in either JIT runtime -- compilation speed is pure
interpreter-side work, so it is one sustained category an interpreter can
win outright.

sxn wins the categories dominated by process startup and one-shot work,
where there is no JIT to warm up, and has by far the tightest worst-case
pause. On Buffer throughput it is now ahead of Bun and close behind Node. It
beats Node on TextEncoder but not Bun, and trails both on EventEmitter --
roughly half of that figure is running the listener's own bytecode, which
nothing short of a JIT removes.

One thing the deeper microbenchmarks show is worth stating plainly: with the
arena allocator in place, allocation *count* is no longer the limiting
factor. An escaping-allocation test puts `{}` at 29.5 ns here against Bun's
7.5 ns and Node's 14.3 ns, while `new ArrayBuffer(40)` is 54.9 ns against
Bun's 51.4 ns and Node's 109.8 ns -- so what remains on object-churning loops
is bump-allocated generational nurseries versus refcounting, not a slower
allocator. That is the same design tradeoff that produces the worst-case
pause figure above.

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

Two later changes carried this further: skipping the per-object property
array for property-less shapes (`new Uint8Array(40)` went from 7 allocations
and 372 bytes to 5 and 308; `{}` from 2 allocations to 1), and dispatching
`Buffer#toString` on an interned atom rather than a chain of string compares.

Cumulatively: Buffer 102->25.9 ms, TextEncoder 76->18.3 ms, EventEmitter
37->20.7 ms, with zero GC cycles during the loops throughout.

What's left in the EventEmitter gap is the interpreted-bytecode floor
itself: ~11.6 ms of that 20.7 ms is just invoking the listener closure 500k
times, which no amount of work outside the interpreter can remove. Closing
that means giving ArcSX a JIT -- its own multi-month project, not a
benchmark tuning pass.
