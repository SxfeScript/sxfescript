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

## Benchmarks: sxn vs Node

`benchmarks/wintercg/run.sh` runs matched WinterCG-style `.sx`/`.js` pairs
against `sxn` and Node side by side, across four categories. No category is
hidden -- Node wins the ones you'd expect it to.

```sh
sh benchmarks/wintercg/run.sh
```

Median of 5+ runs, macOS 26.6.2 (arm64), Node v25.2.1:

| Category | sxn | Node | Winner |
|---|---|---|---|
| Real-world end-to-end task (wall clock, as invoked) | 26 ms | 92 ms | sxn, ~3.5x |
| Cold start | 9 ms | 42 ms | sxn, ~4.7x |
| Sustained throughput: TextEncoder | 23.5 ms | 38 ms | **sxn, ~1.6x** |
| Sustained throughput: Buffer ops | 36 ms | 23.5 ms | Node, ~1.5x |
| Sustained throughput: EventEmitter | 24 ms | 5.0 ms | Node, ~4.8x |
| Pause consistency: worst single pause | 0.05-0.06 ms | 0.20-0.57 ms | sxn |
| Pause consistency: total time | 0.41 s | 0.24 s | Node, ~1.7x |

sxn wins tasks dominated by process startup and one-shot work, where there's
no JIT to warm up, and now wins TextEncoder throughput outright. Node's V8
still wins the remaining hot loops, where its JIT has time to kick in --
QuickJS is an interpreter, not a JIT, by design.

The throughput rows reflect a series of ArcSX/runtime optimizations (all
tagged `arcsx:` in `third_party/quickjs`), roughly in order of payoff:

- **Arena allocator**, ported from upstream quickjs-ng. Small objects come
  from per-size arenas instead of individual `malloc`s, and the refcount/GC
  header moved into the allocation block header. Each `Buffer.from(...)`
  pass allocated 7-8 blocks; recycling them is what took Buffer 83->36 ms
  and TextEncoder 65->23.5 ms in a single change, and cut the pause
  benchmark's total time from 1.1 s to 0.41 s.
- **Typed-array property fast path**: property names that provably can't be
  numeric indices (`.toString`, `.toHex`) stay on the interpreter's inline
  lookup path instead of bailing to the generic exotic-object path.
- **Property lookup cache**: a generation-stamped `(shape, atom)` table
  mapping a lookup to its holder's prototype depth and slot index, so a
  repeated named lookup costs pointer derefs instead of a hash probe per
  prototype level. Worth ~20% on deep prototype chains (idiomatic class
  code) and ~5% here. Only the *location* is cached, never a value, and the
  generation is bumped at every point that can move a property, so a stale
  entry can't be read.
- **One-pass UTF-8 encoding** straight into the final buffer
  (`JS_NewUint8ArrayFromString` / `JS_NewArrayBufferFromString`), a native
  `Buffer.from(str, "utf-8")` that skips the JS subclass-constructor round
  trip (`JS_NewUint8ArrayWithProto`), `TextEncoder.prototype.encode` bound
  directly to its C primitive, and atom-identity event-type lookup plus
  direct fast-array listener access in `EventEmitter` (`JS_GetFastArray`).

Cumulatively: Buffer 102->36 ms, TextEncoder 76->23.5 ms, EventEmitter
37->24 ms, with zero GC cycles during the loops throughout.

What's left in the EventEmitter gap is the interpreted-bytecode floor
itself: ~11.6 ms of that 24 ms is just invoking the listener closure 500k
times, which no amount of work outside the interpreter can remove. Closing
that means giving ArcSX a JIT -- its own multi-month project, not a
benchmark tuning pass.
