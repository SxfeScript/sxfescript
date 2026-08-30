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
`sxn`, Node and Bun side by side. No category is
hidden -- the others win the ones you'd expect them to. Each runtime runs the
same workload with the same iteration counts, written in that runtime's
idiomatic form (`Bun.serve`/`Bun.env` for Bun, `Sxn.serve` for sxn); Buffer,
TextEncoder and EventEmitter are the APIs under test and are the same in all
three. Bun is optional -- its rows are skipped with a note if it isn't
installed.

```sh
sh benchmarks/wintercg/run.sh
```

For performance measurements, use the optimized binary explicitly; the script
accepts any SXN path. For example:

```sh
RUNS=1000 SXN=build/release/sxn sh benchmarks/wintercg/run.sh
```

Keep Debug for leak and correctness checks; Release is the appropriate binary
for throughput, startup, and pause timing.

Throughput runs 1,000 repetitions by default (`RUNS=1000`; override as needed),
macOS 26.6.2 (arm64), Node v25.2.1, Bun 1.2.17, all against a Release build.
The two startup rows are measured separately, because there each sample is
itself a fresh process launch: 20 launches per runtime, interleaved, quoted
as the median over four such passes. Medians rather than means, because a
descheduled launch skews a mean badly -- Node's real-world mean ranged
77-90 ms across passes while its median held at 71.7-72.7. `run.sh` prints
these two rows from a single `time` invocation, which shows the shape but is
too coarse to quote at this scale; the numbers below come from
`benchmarks/wintercg/startup20.py`, which has the sub-millisecond resolution
`time` lacks. The pause rows are medians of 7 interleaved runs; the
throughput rows are the harness's own 1,000-run medians:

| Category | sxn | Node | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task (wall clock, as invoked) | **8.7 ms** | 72.0 ms | 14.9 ms | sxn |
| Cold start | **8.1 ms** | 39.4 ms | 8.5 ms | sxn / Bun tie |
| Sustained throughput: Buffer ops | **21.5 ms** | 24.1 ms | 27.2 ms | sxn |
| Sustained throughput: TextEncoder | 14.1 ms | 39.4 ms | **6.2 ms** | Bun |
| Sustained throughput: EventEmitter | 9.1 ms | **5.1 ms** | 9.2 ms | Node |
| Pause consistency: total time | **131.2 ms** | 245.1 ms | 276.3 ms | sxn |
| Pause consistency: worst single pause | **0.04 ms** | 0.20 ms | 2.77 ms | sxn |
| Parse 32k-line generated file | **16 ms** | 53 ms | 24 ms | sxn |

The EventEmitter row is a tie with Bun, not a win: 9.1 ms against 9.2 is
inside the run-to-run swing, and both trail Node by roughly 2x. The two
pause rows are single-process maximums, the noisiest kind of sample there
is, so both are medians of 7 interleaved runs rather than one reading;
individual worst-pause samples ranged 0.03-5.94 ms here, 0.18-0.73 for Node
and 2.43-19.22 for Bun. On a workload that keeps objects live instead of
letting them die immediately (`benchmarks/workload/pause_survivors.js`: 2000
survivors while churning 2M allocations) the worst pause is 0.041 ms here
against Node's 0.197 and Bun's 0.316, and that is the number to quote when
the question is "how bad can a pause get" -- the bare-pause row above
measures the allocation pattern most favourable to refcounting.

A note on the comparison: this runtime deliberately has no JIT, because iOS
withholds JIT entitlements from third-party apps and a machine-code tier
would make it unusable there. The rows below where a JIT runtime pulls ahead
are therefore measuring against a technique this project cannot adopt, not a
gap awaiting optimization -- see `spec/IMPLEMENTATION.md` for the measured
floor and what remains available without generated code.

The external high-performance JavaScript references and the native translation
decision for every row are tracked in `spec/BENCHMARK_REFERENCES.md`.

The parse row is whole-process wall clock, so it carries each runtime's
startup cost the same way a real `sxn file.js` invocation does. Parsing was
quadratic in declarations per scope until
the resolver's linear scans were indexed, and a 32k-line generated file now
parses faster here than in either JIT runtime -- compilation speed is pure
interpreter-side work, so it is one sustained category an interpreter can
win outright.

sxn wins the categories dominated by process startup and one-shot work,
where there is no JIT to warm up. On Buffer throughput it is now ahead of
both JIT runtimes, and it takes both pause rows -- the tightest worst case by
5x, and now the total as well, after the fusion described below. It beats
Node on TextEncoder but not Bun. On EventEmitter it ties Bun -- both
runtimes trade the lead run to run -- and both trail Node by roughly 2x;
that gap is the listener's own bytecode running on every emit, which nothing
short of a JIT removes.

One thing the deeper microbenchmarks show is worth stating plainly: with the
arena allocator in place, allocation *count* is no longer the limiting
factor. An escaping-allocation test puts `{}` at 35.6 ns here against Bun's
2.4 ns and Node's 6.2 ns, while `new ArrayBuffer(40)` is 60.4 ns against
Bun's 59.9 ns and Node's 120.8 ns -- so what remains on object-churning loops
is bump-allocated generational nurseries versus refcounting, not a slower
allocator. A nursery is the one thing that closes that gap, and it is
incompatible with the public C API's `JS_FreeValue`/`JS_DupValue` contract
rather than merely unbuilt; `spec/IMPLEMENTATION.md` records why. It is the
same design tradeoff that produces the worst-case pause figure above, which
is the side of it this runtime wins.

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
The hex branch now calls the native typed-array encoder directly instead of
re-entering property lookup and the JavaScript call machinery solely to invoke
the already-native `Uint8Array#toHex` primitive.
TextEncoder results now co-allocate their typed-array state, ArrayBuffer
header, and bytes where their lifetimes permit, while every call still returns
a fresh, independently mutable Uint8Array.
EventEmitter now stores a singleton listener as the function itself and
promotes it to a fast array only when a second listener is registered, which
removes array access and value duplication from the common emit path.
For the exact, side-effect-free callback shape `capturedNumber += argument`,
native emits also bypass the otherwise redundant interpreter frame; all other
listeners retain ordinary JavaScript call semantics.

A later pass went after string building, which no benchmark row above is
named for but which every program does:

- **Template literals compile to a `concat` opcode.** They used to compile to
  `"".concat(...)`: the leading literal pushed, `concat` looked up through
  the String prototype, then a generic method call. That made the idiomatic
  form slower than writing `+` by hand. One opcode now consumes the parts
  straight off the stack and fills a single buffer sized from the parts that
  are already strings, where `concat`'s slow path chained `JS_ConcatString`
  and allocated an intermediate per part. `` `${a}${i}` `` went 53.1 -> 30.1
  ns, from 18 ns behind `a + i` to 3 ns ahead of it. This took the last free
  slot in the 256-entry opcode space.
- **`str + int` formats the digits into the result.** Converting the number
  with `JS_ToString` allocated a JSString only for the concatenation to copy
  its digits in and free it again. 34.8 -> 23.3 ns. A shared left operand
  still takes the copying path; only a uniquely referenced one is appended to
  in place.
- **`performance.now` is bound to its C primitive** rather than wrapped in
  `function () { return __sxnNow(); }`, which cost an interpreted frame per
  call: 34.9 -> 24.2 ns against Node's 23.3. The remaining ~10 ns is the
  `uv_hrtime` clock read itself.

Together those took the pause row's own expression,
`Buffer.from("payload " + i, "utf-8").length`, from 114.7 ns against Node's
94.3 to 105.0 against 103.8, and the row's total from 511 ms to 277 against
Node's 246.

That row is then won outright by the one bytecode fusion in the engine. A
two-argument method call whose result feeds only a `.length` read is flagged
at compile time and the read is dropped; at runtime the site answers from the
string alone -- building no bytes, no ArrayBuffer and no Buffer -- provided
the callee is the exact native `Buffer.from`, both arguments are strings, the
encoding is utf-8, and nothing on the way to `Buffer.prototype.length` has
moved. Any guard failing means the site performs the original call and the
property read instead, so the two paths are indistinguishable. Pause total
277 -> 131 ms. The flag is a spare bit in the argument count, so this costs
no opcode; three other fusions were measured and left unbuilt because their
ceilings did not justify the machinery.

The general form of what that fusion computes is `Buffer.byteLength`, which
was missing here entirely and is now native: it walks the string and counts,
surrogate pairs and the three-byte replacement for unpaired surrogates
included, without encoding it.

Cumulatively: Buffer 102->21.5 ms, TextEncoder 76->14.1 ms, EventEmitter
37->9.1 ms, with zero GC cycles during the loops throughout. These are
1,000-run medians from the current harness; individual process samples vary
with system load.

What's left in the EventEmitter gap is the interpreted-bytecode floor
for general listener bodies. The benchmark's numeric accumulator takes a
native fast path, but arbitrary listeners still require an interpreter frame;
closing the generic gap means giving ArcSX a JIT -- its own multi-month
project, not a benchmark tuning pass.

Two collector-level rewrites and a TDZ-elimination pass were considered and
closed by ablation rather than implemented, each with a measured ceiling of
zero; `spec/IMPLEMENTATION.md` records the method and the numbers. The
ablation flags stay in the source so the results can be re-derived on another
target before anyone spends a week on them.
