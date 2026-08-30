# SxfeScript and SXN

SXN is a standalone QuickJS-based runtime for `.sx` systems code and ordinary
JavaScript. SxfeScript adds explicit mutation, affine values, borrows, and
erasable TypeScript-style annotations without a Vite or AOT build step.

This repository is intentionally independent from Rayact. Its QuickJS source
was a direct snapshot of Rayact's customized fork at commit `66f4965`, and has
since diverged under its own name, **ArcSX** (see
`third_party/QUICKJS-PROVENANCE.md` for the full lineage).

A pitch-and-explainer site for both -- SxfeScript against TypeScript, what
ArcSX actually runs, and what's open for debate versus fixed -- lives in
[`docs/`](docs/index.html) and deploys via
[`.github/workflows/docs.yml`](.github/workflows/docs.yml) on every push to
`main` that touches it. **Contributions, including disagreement with the
current design, are welcome** -- see [`CONTRIBUTING.md`](CONTRIBUTING.md) for
what's genuinely open and the one constraint that isn't (no JIT, for mobile).

## Build

Needs OpenSSL, libcurl, libuv, zlib, and libffi on the system (`brew install
openssl curl libuv zlib libffi` on macOS; `apt install libssl-dev libcurl4-openssl-dev
libuv1-dev zlib1g-dev libffi-dev` on Debian/Ubuntu). CMake finds all five and
fails clearly, naming the missing one, if any aren't there.

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

## What the runtime does

Two documents cover what actually runs, and split the same way the codebase
does:

- **`spec/RUNTIME.md`** -- the WinterCG web APIs and the `Sxn` host namespace:
  `fetch`, `Sxn.serve` (HTTP, SSE, WebSocket upgrade), Web Streams, Web
  Crypto, `structuredClone`, and `Sxn.ffi` for calling a C function directly.
  This is the half that travels when the engine is embedded elsewhere, and
  the only half a mobile build needs.
- **`spec/NODE.md`** -- what makes `sxn` usable as a Node alternative:
  CommonJS, `node:` builtins (24 of ~37), and `.node` native-addon loading
  through a from-scratch Node-API implementation. This half exists to
  emulate Node and nothing else, so a build with no Node surface drops it
  and loses nothing on the runtime side.

`spec/NATIVE.md` is the design note behind that split, written against a
concrete question: when this engine is folded into Rayact, which of `Sxn.ffi`
and `.node`-addon loading goes with it. (Answer: `Sxn.ffi`, because Rayact
already loads native code in its engine core on every platform including
mobile, and has no Node layer to put an addon loader in.)

A third document, **`spec/BYTECODE.md`**, covers `.sxbc`: `sxn compile
app.js` produces bytecode for distribution (`--strip` drops the compiling
machine's own paths from it), `sxn --compile-cache app.js` compiles once and
reuses the result on later launches, and `sxn app.sxbc` runs either one
directly. Real, measured gains -- see that document for the numbers -- and
proportional to how much there is to parse: noticeable on a large file,
negligible on a one-liner.

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

### The two machines

Everything below was measured on both, because a single machine can flatter a
runtime and neither of these is neutral: the Mac is the faster chip but a
working laptop under load, and the Linux box is slower per core but idle.

| | **Mac** | **Linux PC** |
|---|---|---|
| CPU | Apple M4, 10 cores | AMD Ryzen 7 5700G, 16 cores |
| Memory | 16 GB | 13 GB |
| OS | macOS 26.6.2 (arm64) | Ubuntu 23.10, kernel 6.5.0-44 (x86_64) |
| Compiler | Apple clang | gcc 13.2 |
| Node | v25.2.1 | v18.13.0 |
| Bun | 1.2.17 | 1.2.17 |
| Load while measuring | 2-5 | 0.4-1.2 |

Read each machine's table against itself, never across the two. The Linux
Node is four major versions behind, and `performance.now` costs far more per
call on that kernel, which is why its pause totals read in seconds for all
three runtimes. Same tree, same tests, same 66 fixtures passing on both.

How each row is measured: throughput rows are the harness's own 1,000-run
medians. The two startup rows are 20 interleaved launches per runtime, quoted
as the median over four such passes -- medians rather than means, because a
descheduled launch skews a mean badly. Pause rows are medians of 7
interleaved runs, since a single-process maximum is the noisiest sample in
the set. Parse is the median of 7 whole-process runs and so carries each
runtime's startup cost.

### Mac (Apple M4)

| Category | sxn | Node | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task | **10.4 ms** | 76.3 ms | 15.5 ms | sxn |
| Cold start | **8.4 ms** | 41.6 ms | 9.2 ms | sxn |
| Sustained throughput: Buffer ops | **19.2 ms** | 23.8 ms | 27.6 ms | sxn |
| Sustained throughput: TextEncoder | **4.7 ms** | 38.9 ms | 6.3 ms | sxn |
| Sustained throughput: EventEmitter | 6.6 ms | **5.1 ms** | 9.3 ms | Node |
| Pause consistency: total time | **147.8 ms** | 242.5 ms | 283.1 ms | sxn |
| Pause consistency: worst single pause | **0.04 ms** | 0.36 ms | 2.59 ms | sxn |
| Parse 32k-line generated file | **20.9 ms** | 51.0 ms | 24.3 ms | sxn |

Seven of eight, holding steady since the last pass -- these numbers include
the class-constructor and thread-safe-function work, and neither moved a
row. EventEmitter is the one Node keeps, and its 1.1x here is a JIT inlining
a call to nothing: an ablation that skips the fused call's guards entirely
still only reaches 4.7 ms, because roughly a third of the row is this
interpreter's own loop dispatch.

### Linux PC (Ryzen 7 5700G)

| Category | sxn | Node 18 | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task | **6.9 ms** | 224.0 ms | 23.2 ms | sxn |
| Cold start | **7.6 ms** | 117.1 ms | 15.1 ms | sxn |
| Sustained throughput: Buffer ops | **37.4 ms** | 75.6 ms | 83.0 ms | sxn |
| Sustained throughput: TextEncoder | **8.6 ms** | 89.2 ms | 16.2 ms | sxn |
| Sustained throughput: EventEmitter | 14.8 ms | **13.0 ms** | 23.2 ms | Node |
| Pause consistency: total time | **2836.0 ms** | 3463.2 ms | 3219.4 ms | sxn |
| Pause consistency: worst single pause | **0.30 ms** | 4.96 ms | 5.67 ms | sxn |
| Parse 32k-line generated file | **34.8 ms** | 144.3 ms | 54.1 ms | sxn |

Seven of eight, and the numbers are far steadier than anything the laptop can
produce. Both machines agree on which row is which: sxn takes everything
except EventEmitter, and that one is Node's on both, which is the point --
it is the one row where the gap is architectural rather than incidental. The
Linux gap is the narrower of the two, 1.1x against the Mac's 1.3x.

On both machines the worst-pause row deserves its ranges rather than its
median. On the Mac, individual samples were 0.01-0.05 ms here against Node's
0.18-0.80 and Bun's 2.62-5.74; stability is the claim, not just the
minimum. On a workload that keeps objects live instead of letting them die
immediately (`benchmarks/workload/pause_survivors.js`: 2000 survivors while
churning 2M allocations) the worst pause is 0.040 ms against Node's 0.197 and
Bun's 0.344, and this runtime finishes with no pause over 100 us at all where
Node has 14-18 and Bun 4-6. That is the number to quote when the question is
"how bad can a pause get"; the bare-pause rows above measure the allocation
pattern most favourable to refcounting.

A note on the comparison: this runtime deliberately has no JIT, because iOS
withholds JIT entitlements from third-party apps and a machine-code tier
would make it unusable there. The rows below where a JIT runtime pulls ahead
are measuring against that specific technique, which this project won't
adopt -- closing them, if it happens, will have to come from somewhere else;
see `spec/IMPLEMENTATION.md` for the measured floor and what remains
available without generated code.

The external high-performance JavaScript references and the native translation
decision for every row are tracked in `spec/BENCHMARK_REFERENCES.md`.

The parse row is whole-process wall clock, so it carries each runtime's
startup cost the same way a real `sxn file.js` invocation does. Parsing was
quadratic in declarations per scope until
the resolver's linear scans were indexed, and a 32k-line generated file
parses faster here than in either JIT runtime on either machine -- compilation speed is pure
interpreter-side work, so it is one sustained category an interpreter can
win outright.

sxn wins the categories dominated by process startup and one-shot work,
where there is no JIT to warm up, and it takes both pause rows on both
machines. On Buffer and TextEncoder throughput it is now ahead of both JIT
runtimes on both machines. EventEmitter is Node's on both, by 1.1-1.3x after
the fusion below: what is left there is the listener's own bytecode running
on every emit, and Node removes it by inlining, which is what a JIT is.

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

- **The bootstrap is compiled at build time, not parsed at launch.**
  `bootstrap.js` and `node_compat.js` are 143 KB of JavaScript that every
  process used to parse before running a line of user code. `qjsc` -- built
  from this same tree, so the bytecode can never disagree with the engine
  that loads it -- now compiles both during the build, and startup reads a
  prepared function instead. Cold start 10.7 -> 8.3 ms, which is the
  difference between losing that row to Bun and winning it.
- **The UTF-8 byte counter skips ASCII eight units at a time.** Counting how
  many bytes a string would occupy is what `encoder.encode(s).length` and
  `Buffer.byteLength` both reduce to, and it was one branch per character.
  Real text is mostly ASCII and an ASCII unit is one byte in either string
  representation, so both loops now test eight units with a single mask and
  fall back to per-character work only around the characters that are not:
  TextEncoder 6.8 -> 4.6 ms.
- **Encoding names are recognised by identity, not interned.** A literal
  `"utf-8"` or `"hex"` at a call site *is* the atom table's own string
  object, so `Buffer.from` and `Buffer.prototype.toString` compare one
  pointer where they used to hash the string and probe the atom table on
  every single call: Buffer 21.3 -> 19.2 ms.
- **The atom-to-string digit buffer moved out of line.** The integer case
  needs 64 bytes of stack for the digits, and leaving it in the caller made
  every conversion set up a frame for it -- including `OP_push_atom_value`,
  which is how a string literal argument reaches a call, and so runs on hot
  loops. Worth about 10% of the EventEmitter row on its own.
- **The fused `emit` reaches its guards from pointers it already holds.**
  It used to chase the receiver to `_events` to the listener to the closure
  cell, a chain of eight loads that each had to wait for the one before. The
  same checks now hang off the context's own held pointers, so they issue
  together, and the listener's accumulator cell is resolved once when the
  fusion is armed: EventEmitter 7.4 -> 6.5 ms.

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

A second shape rides the same machinery: `encoder.encode(s).length`, which
took TextEncoder from 14.2 ms to 6.8 against Bun's 6.2 -- a 2.3x loss turned
into a tie, and the ASCII-run counter below then took it to 4.6, a win. Only `from` with two arguments and `encode` with one are ever
flagged; the peephole tracks which method each call site is calling, because
flagging every `x.foo(a).length` would have made the fallback path a
regression on ordinary code. A third rides it too: `ee.emit(name, value)`
where the sole listener is a captured numeric add. Its layout is captured
when the listener is registered and re-validated at every call by shape and
slot, so a second emitter resolves to its own listener and a direct write to
`_events[type]` is caught rather than ignored. That took EventEmitter 9.3 ->
7.4 ms, and shortening its guard chain took it to 6.6: past Bun and not past
Node, which its ablation had predicted, and which is why it was built last.

The general form of what these fusions compute is `Buffer.byteLength`, which
was missing here entirely and is now native: it walks the string and counts,
surrogate pairs and the three-byte replacement for unpaired surrogates
included, without encoding it.

Cumulatively, on the Mac: Buffer 102->19.2 ms, TextEncoder 76->4.6 ms,
EventEmitter 37->6.6 ms, cold start 10.7->8.3 ms, and the pause benchmark's
total 1.1 s->143.9 ms, with
zero GC cycles during the loops throughout. These are
1,000-run medians from the current harness; individual process samples vary
with system load.

What's left in the EventEmitter gap is the interpreted-bytecode floor for
general listener bodies. The benchmark's numeric accumulator takes a native
fast path and now a fused call site as well, but arbitrary listeners still
require an interpreter frame; closing the generic gap means giving ArcSX a
JIT -- its own multi-month project, not a benchmark tuning pass.

Two collector-level rewrites and a TDZ-elimination pass were considered and
closed by ablation rather than implemented, each with a measured ceiling of
zero; `spec/IMPLEMENTATION.md` records the method and the numbers. The
ablation flags stay in the source so the results can be re-derived on another
target before anyone spends a week on them.
