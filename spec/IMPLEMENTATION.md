# Implementation ledger

## Implemented foundation

- Standalone CMake project and ArcSX QuickJS fork (originally a direct
  snapshot of Rayact's customized fork; see
  `third_party/QUICKJS-PROVENANCE.md`).
- `.sx`, `.js`, `.mjs`, and `.cjs` CLI entrypoints.
- Native QuickJS parsing of `.sx` syntax -- interfaces, type annotations,
  `let mut`, `safe`, `unsafe`, and `&`/`&mut` borrow sigils -- with no
  separate transform step. The earlier in-memory text transformer
  (`sxfe_compile`, src/frontend.c) remains as an independently unit-tested
  component but is no longer on the execution path.
- One ownership rule is enforced rather than erased: `&mut x` requires `x` to
  be a `let mut` owner, and borrowing an immutable one is a parse error naming
  SX2003 (`js_parse_unary`, third_party/quickjs/quickjs.c). The check rules
  only on a bare identifier it can resolve in the current function's lexical
  scope chain or in its top-level lexicals; a parameter, a captured outer
  binding, or anything it cannot resolve is left alone rather than guessed at.
  Guarded by the `sxn-reject-mut-borrow` ctest. Note that `&mut` is still
  erased at runtime, so it aliases through JS object identity and cannot write
  back to a caller's number -- that needs the ownership CFG below.
- Declared types are recorded rather than discarded.
  `js_parse_type_annotation` classifies while it skips, and the result is kept
  on `JSVarDef.sx_type` -- which covers parameters too, since `fd->args` is a
  `JSVarDef[]` and reaches the bytecode through the `vardefs` memcpy -- and as
  `JSFunctionBytecode.sx_ret_type`. Only the scalar types codegen acts on are
  named. A union, a generic, an inline object type, a borrow, a struct name,
  `string` and `void` all report `SX_TYPE_OTHER`, so codegen never specializes
  on a type it half-understood, and no enum value exists that nothing reads.
  The type is deliberately not propagated to `JSGlobalVar` or `JSClosureVar`:
  both were written and never read, and a `safe` module-level binding is kept
  in `fd->vars` by `sx_safe_module_local` anyway. The skipping itself is
  unchanged, which is what keeps arbitrary erasable TypeScript parsing.
- `safe` specializes on the type that was written, not on the keyword. The i32
  opcodes are gated on `is_safe && sx_type == SX_TYPE_I32`
  (`sx_is_safe_i32`), so `safe let mut x: f64` and an un-annotated `safe let
  mut` keep exact JavaScript arithmetic instead of reaching a wrapping integer
  opcode and being rescued by its runtime tag test. This also settled an
  inconsistency: a constant right-hand side and a local one compile through
  different peephole branches, and `safe let mut n: i32` used to promote on
  `n += 1` while wrapping on `n += step`. Both wrap now.
  `tests/fixtures/typed_overflow.sx` covers all four combinations plus the
  fused loop; `tests/fixtures/js_overflow.mjs` stays the plain-JS boundary.
- Typed calls are inlined. A call whose callee is a local written exactly
  once by an `fclosure`, never captured, with every parameter and the return
  declared scalar, and whose body loads each parameter once in order and then
  runs only operand-free arithmetic, is spliced into the caller at the pass-2
  peephole (`sx_inline_typed_calls`). The measured effect is below. Plain
  JavaScript never qualifies, because the gate is the declared signature.
  `tests/fixtures/typed_inline.sx` asserts the splice changes nothing
  observable -- values, coercions, exceptions, reassignment, capture, wrong
  arity, use as a value -- with every expected value taken from Node.
- Fixed-layout calculation and aligned growable/poisonable arena primitives.
- Module-loader hook that transforms imported `.sx` modules in memory.
- Package command surface with safe argument validation, disabled lifecycle
  scripts, and a bootstrap npm-compatible backend.
- LSP JSON-RPC transport and VS Code language registration.
- Contextual `safe let`/`safe const` compatibility parsing and CLI
  `--memory-report`/`--leak-check` diagnostics using QuickJS accounting.
- `Sxn.ffi` is implemented on libffi and `dlopen`: scalars, pointers and
  NUL-terminated strings, with structs by value, callbacks and variadics
  rejected rather than half-supported. `.node` addons load through a
  Node-API implementation on QuickJS. Which of the two lives in the runtime
  and which in the node: layer, and why, is `spec/NATIVE.md`. Native parsing
  of `unsafe extern` still rejects the declaration with a "not yet
  supported" error rather than mis-parsing it; the standalone compatibility
  transformer lowers it to the `Sxn.ffi` call that now works.
- Native SX execution is unconditional; `SXN_NATIVE_SX` is no longer read
  and has no effect.
- `.sxbc` precompiled bytecode: `sxn compile`, `sxn --compile-cache`, and
  running a `.sxbc` file directly all work, for both module and CommonJS
  entries. `spec/BYTECODE.md` has the format, the measured gains, and the
  trust boundary (bytecode is not a safe format for untrusted input).
- The vendored QuickJS-ng tooling (`qjs`, `qjsc`) reports itself as ArcSX in
  every user-visible banner and in the comment `qjsc` writes atop a
  generated header; `third_party/QUICKJS-PROVENANCE.md` has the lineage this
  is built on.

## Performance shape (measured, benchmarks/wintertc/run.sh)

- sxn wins seven of the eight README benchmark categories on both measured
  machines: startup, cold start, Buffer and TextEncoder throughput, both
  pause-consistency rows, and whole-process parse time. EventEmitter is
  Node's by 1.1-1.3x, and that gap is architectural rather than incidental
  -- see the README for the measurement and `spec/NATIVE.md`'s note on why a
  JIT tier isn't coming.

## Measured performance ceiling (why some gaps are not tunable)

Recorded so this is not re-derived. All figures are the minimum of 4+ runs on
macOS arm64, against Bun 1.2.17; sxn is the Release build.

| Operation | sxn | Bun |
|---|---|---|
| Empty loop iteration | 16.3 ns | 0.4 ns |
| `Math.max(1,2)` (bare native call) | 30.7 ns | 0.4 ns |
| `Object.is(1,1)` | 34.0 ns | 0.4 ns |
| `TextEncoder.encode` (36B ASCII) | 96.4 ns | 22.0 ns |

The decisive line is the second: a JIT inlines `Math.max(1,2)` to a constant,
so Bun's cost for a builtin call rounds to zero, while an interpreter must
dispatch the opcode and push a C frame. That puts a hard floor under every
per-call benchmark:

- sxn's floor for *any* native call in a loop is ~30 ns (16 ns dispatch +
  14 ns call). Bun's entire `TextEncoder.encode` is 22 ns. So even with an
  encoder that took zero time, sxn could at best tie Bun on the 200k-call
  TextEncoder benchmark. It is not reachable by optimizing the encoder.
- The same floor explains EventEmitter: roughly half that benchmark is
  invoking the listener's own bytecode.

Three things were ruled out by measurement along the way, and should not be
retried without new evidence:

- **Allocation count is not the limiting factor.** `encodeInto`, which
  allocates nothing, measures *slower* (130.6 ns) than `encode`, which
  allocates a fresh array (101.3 ns). Reducing `new Uint8Array(40)` from 7
  allocations to 5 moved the TextEncoder benchmark by ~1 ms.
- **The object model is not the gap either.** `new Plain()` (58.7 ns) and a
  bare `{}` (58.3 ns) cost the same, so constructor and prototype machinery
  is not what is being paid for; eliding `.prototype` resolution would gain
  approximately nothing, and memoizing its slot measured ~2%.
- **Recursion depth was a real compatibility bug, not a performance one.**
  QuickJS budgets 1MB of JS stack regardless of the thread's actual limit,
  which capped recursion at 948 frames against Node's 8874. Sizing the budget
  from RLIMIT_STACK with a 2MB reserve raised it to 5682. The reserve is not
  optional: native builtins descend several C frames between the
  interpreter's overflow checks, and overshooting the real stack is a crash
  instead of a catchable RangeError.
- **`ta.length` is not slow.** It measures ~21 ns in a clean loop, of which
  ~11 ns is the loop itself. An earlier figure of 57 ns came from a harness
  that ran several benchmarks back to back and was reading accumulated
  memory pressure, not the operation. An inline fast path for the built-in
  length getter was written, verified and reverted: it changed nothing on
  any real benchmark. Beware benchmarks that share a process with earlier
  ones -- measure each in isolation and take a minimum.

One avenue is deliberately *not* taken, and the reason is a compatibility
boundary rather than performance. A plain JS loop body compiles to 11 opcodes
per iteration (~11 ns), six of them TDZ-checked local accesses; the fork's
fused `OP_add_loc_safe_i32` would collapse the accumulator, and the compiler
gates it on `safe` locals. That gate is load-bearing: the opcode computes
`(int32_t)((uint32_t)left + (uint32_t)right)`, i.e. it *wraps* on overflow,
which is the defined semantics for SX `safe x: i32` but wrong for standard
JavaScript, where `x += 1` at 2^31-1 must promote to a double. Extending i32
inference to plain-JS accumulators would silently corrupt arithmetic.
`tests/fixtures/js_overflow.mjs` guards this boundary. The loop floor is in
any case ~11 ns against per-iteration benchmark costs of 90-100 ns, so
halving it would return roughly 5%.

A pattern worth naming, because it cost several attempts: at this level the
sampling profiler's self-time attribution is not a reliable guide. Four
changes that profiled as 5-13% of a loop measured *neutral* once A/B'd
interleaved on a settled machine -- caching accessors in the property IC, an
inline fast path for the built-in typed-array length getter, memoizing the
`prototype` slot (~2%, kept anyway as it is small and correct), and
memoizing find_hashed_shape_proto's (prototype -> empty shape) lookup. What
did pay was always something the profile named as *real work* rather than
overhead: the mixed int/float arithmetic falling through to js_add_slow, and
the missing let/const compound-assignment fusion. Profile to find candidates,
but only an interleaved minimum-of-N A/B decides.

Two further attempts were reverted for measuring *slower*: caching accessor
properties in the property inline cache (typed-array `.length` 35.3 -> 49.7
ns, because the extra branch on every cache hit costs more than the walk it
saves), and a single-way per-call-site inline cache (12% slower than shape
keying on a four-shape call site).

### The call frame is the one large removable cost, and types remove it

Every ceiling above measured at roughly zero. This one did not. All figures
are the minimum of 9 runs over 5M iterations, macOS arm64, Release, reported
per loop iteration; subtract the empty loop to read the work itself.

| | sxn | Node 25.2 | Bun 1.2.17 |
|---|---|---|---|
| empty loop | 4.0 ns | 0.27 | 0.23 |
| interpreted call, 0 args | 14.4 ns | -- | -- |
| interpreted call, 2 args | 16.3 ns | 0.37 | 0.22 |
| interpreted call, 4 args | 21.3 ns | -- | -- |
| field read + write | 11.9 ns | 0.27 | 0.22 |
| f64 accumulate | 5.9 ns | 0.53 | 0.54 |

Read those the way the ledger reads every JIT comparison: Node and Bun delete
these loops outright, so no interpreter change reaches 0.3 ns. What the table
locates is where *this* runtime's time goes. A call costs ~10.4 ns before an
argument is passed and ~1.7 ns per argument after; a field access is ~2.6 ns;
arithmetic is already within 2 ns of the dispatch floor and has nothing left
in it. Annotations bought none of this before: typed and untyped field access
measured 11.76 and 11.67 ns, the same number.

Hand-inlining gave the exact upper bound for removing the frame:

| | via call | hand-inlined | recovered |
|---|---|---|---|
| `add2(i, 1)`, both `i32` | 17.1 ns | 5.4 | 11.8, 3.2x |
| `len2(v)`, an interface | 28.9 ns | 18.1 | 10.7, 1.6x |

**A cheaper prologue is a negative result.** Before building one, the prologue
was ablated: `-DSXN_ABLATE_CALL_PROLOGUE` removes the stack-overflow check and
the GC-free section, the only two pieces a statically known callee could be
proven not to need. Interleaved over three rounds the 0-argument call went
14.39 -> 12.89 ns and both 2-argument rows landed inside noise. A 1.5 ns
ceiling on the one shape that benefits does not justify branching
`JS_CallInternal` on a signature, so it was closed rather than written. The
rest of the prologue is not ablatable at all -- the `var_buf` fill, the
`var_refs` fill and the realm switch change meaning rather than repeat known
work, and a build without them crashes during bootstrap. The flag stays in
the source so the result can be re-derived on another target.

**Inlining is where the 11 ns was, and it needs no opcode.**
`sx_inline_typed_calls` runs in the pass-2 peephole beside
`fuse_i32_accum_loops` and splices the callee's body over the call, which
matters because the opcode space is full (`static_assert(OP_COUNT == 256)`;
`spec/PERFORMANCE.md` records the template-literal `concat` op taking the last
slot). Measured with `benchmarks/engine/call_inline_probe.sx`:

| | ns/op |
|---|---|
| empty loop | 4.07 |
| untyped call | 16.34 |
| typed call, inlined | 5.31 |
| hand-inlined ceiling | 5.38 |

16.34 -> 5.31 ns, 3.1x, landing on the hand-inlined ceiling. Against Node's
0.37 ns for the same call that is a 44x gap narrowed to 14x -- narrowed, not
closed, and the row should be quoted that way. Compile time did not move:
20000 small functions still compile in 40 ms, and the pass early-outs on any
function with no locals or no constant pool.

The `benchmarks/wintertc` rows are unchanged by all of it (buffer 19.4 ms,
textencoder 4.6, events 6.7, worst pause 0.04), which is expected: none of
those workloads calls a small function with a declared scalar signature.

Two limits worth stating rather than discovering later. The splice requires
the body to load every parameter once in declaration order, so
`(a, b) => a * b + c` and anything reusing a parameter (`v.x * v.x`) still
pays for its frame; lifting that needs the arguments in caller temporaries,
which means allocating locals after `resolve_variables`. And an inlined callee
no longer appears in a stack trace if its arithmetic throws -- the same
tradeoff every inlining compiler makes.

### A JIT is ruled out by platform, not by effort

iOS does not grant W^X/JIT entitlements to third-party apps, so a
machine-code tier would make this runtime unusable on a target platform.
That rules out the one technique -- generating machine code -- that a JIT
uses to close this gap. It does not rule out closing it some other way; the
dispatch floor above is the floor of what's been tried, not a proof that
nothing faster exists. Benchmarks against JIT runtimes should be read with
that in mind: on JIT-bound microbenchmarks the comparison is against a
technique this project won't use, not necessarily a result it can't reach.

### The remaining collector rewrites are measured at ~zero ceiling

The second-opinion review (see below) rated two collector-level designs as
the strongest remaining options: replacing the per-object GC list with
arena-block iteration (est. 8-20 ns of the 76 ns object lifecycle) and
Bacon-Rajan candidate buffering so a 1->0 death never touches the list
(est. 10-25 ns). Both estimates rest on the same premise: that
add_gc_object/remove_gc_object's doubly-linked list maintenance is a
material per-object cost.

An ablation tested the premise directly. Building with
-DSXN_ABLATE_GC_LIST makes add_gc_object self-loop the link instead of
inserting into gc_obj_list, eliminating the list cost entirely; on
workloads verified GC-free (gcCount 0, so the list is never read), the
binary is behaviorally identical and the A/B isolates pure linkage cost.
Result, interleaved minimums: `{}` lifecycle 37.0 -> 37.7 ns, `{a:1}` 63.0
-> 62.7, empty array 45.8 -> 45.7, all three throughput benchmarks within
noise. The ceiling for both rewrites is ~0-1 ns on this allocator: the
insert/remove touches adjacent hot cache lines and is effectively free.

Both designs are therefore closed with a negative result rather than
deferred: multi-week collector rewrites cannot pay when an exact upper
bound on their benefit measures zero. The remaining lifecycle cost sits in
JS_NewObjectFromShape's field initialization, shape refcounting and the
allocator fast path itself, per the profile -- diffuse, not concentrated
behind any single removable structure.

### TDZ check elimination has a measured ceiling of zero

The interpreter re-tests the temporal dead zone on every read of a `let` or
`const`: the emit benchmark's inner loop alone runs four `_check` opcodes per
iteration on bindings that were initialized long before. Eliminating those
statically needs a dataflow pass -- a fixed point over the CFG, intersecting
initialized-sets at join points -- which is correctness-critical in the worst
way, because a bug does not crash. It silently stops throwing ReferenceError
and the engine quietly accepts programs the spec rejects.

Ablation settles whether that risk is worth taking. Building with
-DSXN_ABLATE_TDZ=1 skips the JS_IsUninitialized test in OP_get_loc_check and
OP_get_var_ref_check while leaving the opcode, its operand and dispatch
untouched, so the A/B isolates exactly the branch a perfect elimination pass
would remove -- an exact upper bound, and behaviourally identical on code
that never trips TDZ.

The bound is nothing. Interleaved minimums: buffer 20.9 -> 20.8 ms,
textencoder 13.7 -> 13.6, events 8.9 -> 8.7, and on the real workloads
text.js 16.9 -> 17.1, config.js 207.7 -> 209.0, collections.js 41.6 -> 41.7,
i.e. inside noise and signed the wrong way as often as not. A synthetic loop
doing eight lexical reads per iteration does show 0.17 ns per check, which is
what made the lever look worth pulling; real code does not read the same
binding eight times per iteration, and the branch is perfectly predicted
not-taken, so the test disappears into the load it accompanies.

Closed as a negative result. The ablation flag stays in the source so the
measurement can be re-derived on another target before anyone spends a week
on the pass.

What remains available is everything that needs no generated code:

- **Direct dispatch of C-function calls** (done): calling js_call_c_function
  straight from OP_call/OP_call_method rather than through JS_CallInternal's
  prologue cut native-call overhead 14.5 -> 8.4 ns.
- **Quickening / type specialization** in the CPython PEP 659 sense --
  rewriting opcodes in place once their operand types are observed. Pure
  interpreter bookkeeping. Note the hazard recorded below: the fork's
  existing fused i32 opcodes wrap on overflow and must stay gated on `safe`.
- ~~**Tail-call dispatch** of the interpreter loop (musttail + preserve_none)~~
  -- measured and rejected. Apple clang 21 on arm64 supports both
  `[[clang::musttail]]` and `preserve_none`, and a spike modelling both
  dispatch styles over the same opcode mix
  (`benchmarks/engine/dispatch_bench.c`) puts musttail **47-70% slower** than
  the computed-goto threading quickjs already uses, repeatably at -O2 and
  -O3. CPython's reported 10-15% was measured against a baseline that was not
  using computed goto. This would have been a multi-week restructuring of the
  interpreter into per-opcode functions for a large regression.
- **A generational nursery**, which is not a JIT and would address the
  teardown cost -- but see the tradeoff below.

The remaining item is a generational nursery for object churn
(freeing a small object costs ~29 ns here against Bun's ~1 ns, which is
refcounting versus a nursery that reclaims dead young objects for free).
On the nursery's cost, an earlier claim in this ledger was overstated and is
corrected here. The 0.05 ms vs 2.62 ms worst-pause figure comes from
`benchmarks/wintertc/pause.sx`, where every allocation dies immediately --
the pattern that most flatters refcounting and most penalises a collector,
which must still scavenge. Re-measured on `benchmarks/workload/
pause_survivors.js`, which keeps 2000 objects live while churning 2M, the
gap nearly closes:

| worst pause | sxn | Node | Bun |
|---|---|---|---|
| allocations die immediately | 0.04 ms | -- | 2.53 ms |
| 2000 live survivors | 0.099 ms | 0.203 ms | 0.241 ms |

Two to three times better on the realistic pattern, not fifty. Note also
that this runtime records far *more* small gaps (348 over 10 us against
Bun's 51) -- refcounting spreads its cost rather than avoiding it.

So the pause argument against a nursery is much weaker than this ledger
first claimed. The argument that remains is scope, and it is a different
one: a nursery is not a bounded change for this engine.

An empty `{}` costs 76 ns to create and destroy here against Node's 3.8 ns,
and it has no properties, so refcount *cascades* are not the cost. What is
left is the per-object lifecycle itself -- allocation, linking into the GC
object list, taking and releasing a shape reference, arena free -- spread
across the design rather than sitting in one hotspot. Reaching a few
nanoseconds means not doing that work per object, which means bump
allocation and reclaiming young objects without individual frees.

That is incompatible with how QuickJS works in two ways that matter. A
*moving* nursery cannot be added while raw JSValues live on the C stack and
in embedder variables; there is no handle layer to update. A *non-moving*
young generation still has to determine liveness without refcounts, and
refcounting is not an implementation detail here -- JS_FreeValue is in the
public C API, so every embedder depends on it.

The accurate framing is therefore not "a multi-week project we have not
scheduled" but "a different engine". Recorded so the option is neither
dismissed for the wrong reason (pause latency, which was overstated) nor
adopted under the wrong assumption (that it is incremental work).

## Finding real defects: the complexity probe

The benchmark suite measures seven workloads and missed four genuine defects
that ordinary code hits hard. All four were found instead by probing the
*shape* of cost curves -- per-operation cost measured at two collection sizes,
flagging anything whose ratio suggests super-linear behaviour -- and by
comparing per-op cost against Node, flagging anything far past the ~4x
baseline interpreter gap. The probes are checked in:

- `benchmarks/engine/complexity_probe.js` -- ratio of per-op cost at two sizes
- `benchmarks/engine/op_probe.js` -- absolute per-op cost, wide operation mix
- `benchmarks/engine/string_probe.js` -- string and regexp operations
- `benchmarks/engine/dispatch_bench.c` -- interpreter dispatch styles

What they found, all since fixed: Map/Set lookup was O(n) for integer keys
and again for object keys (degenerate hashes; 4096 keys in 8 and 64 buckets
respectively), `Array#splice` permanently converted any array it removed
from into a slow array (~85x on the next splice, and it never recovered),
and `Array#includes`/`#indexOf` called a generic comparison per element.

They also cleared a good deal, which is worth as much: string append is
correctly amortized O(1), object property reads, array indexing, push/pop
and string slicing all scale flat, and global regexp replace is linear in
match count (~250 ns/match against Node's ~25 -- a constant factor from the
pre-rewrite regexp engine, not a defect; upstream's register-based engine
was skipped in the cherry-pick and remains available).

Two probe flags were the probe's own fault and are not defects:
`String#indexOf` and `JSON.stringify` genuinely scale with input size.

The lesson worth carrying: a fixed benchmark suite measures what it was
written to measure. Sweeping for anomalous *shapes* found in one sitting
several defects that mattered more to real programs than any benchmark row
in the suite.

## Fixed: parsing was quadratic in declarations per scope

A 32k-line file of top-level `let`s took 1.03 s against Node's 0.05 s,
growing quadratically. The cause was five separate linear scans, each run
once per declaration or reference, fixed in two passes:

- Parse path: `find_var_in_child_scope` and `find_global_var` (the latter
  given the same open-addressed index `find_var` already had).
- Resolution path: `resolve_scope_var`'s scope-chain walk (indexed by a
  name -> single-declaration table with ancestry check via is_child_scope;
  duplicates, pseudo-vars and `_with_` functions fall back to the walk);
  `find_closure_var` (indexed, maintained on append, first-match order
  preserved); and the three per-global-variable scans of the closure list in
  resolve_variables and instantiate_hoisted_definitions (answered by the
  closure index whenever no `_var_`/`_arg_var_`/`_with_` pseudo entries
  exist -- they only appear for direct eval -- since without them the scans
  reduce to first-match name lookups).

Result: 32k lines 1.03 s -> 0.01 s, 60k declarations 2.63 s -> 0.02 s --
from 20x slower than Node to ~5x faster. Guards:
`tests/fixtures/declaration_scoping.mjs` (redeclaration errors, shadowing,
TDZ, closure capture) and `tests/fixtures/eval_scopes.cjs` (direct-eval and
`with` resolution, the pseudo-variable paths the indexes must never
shortcut), both byte-identical to Node.

## Known: object literals rebuild their intermediate shapes

Building `{a: 1, b: 2}` transitions empty -> {a} -> {a,b}. The shape hash
table holds no reference, so a shape dies with its last object -- and the
intermediate {a} has no holder at all. It is created, hashed, used for one
property store, then freed, on every literal.

Measured with `benchmarks/engine/shape_churn_probe.js`, which keeps an
`{a:0}` object alive from JS for no reason other than to pin that shape:
`{a,b}` literal creation goes 103.1 -> 78.7 ns, about 24%, on one of the
most common operations in any program. Node builds the same literal in
~4 ns.

An attempt to fix this with a bounded keep-alive ring of recently hashed
shapes **segfaults**, and the reason is worth recording. In add_property's
transition path the freshly cloned shape is used under
`assert(JS_REF_COUNT(p->shape) == 1)`: add_shape_property mutates it in
place precisely because it is known to be unshared. Taking a reference for a
cache makes it shared, and the in-place mutation then corrupts a shape other
objects can reach. Any fix has to take its reference *after* the shape is
complete, or make the table an owner and let the cycle collector reclaim
shape->proto->shape cycles -- not simply pin the shape mid-transition.

The ad-hoc pinning of Buffer/Uint8Array/ArrayBuffer shapes in
`sxn_pin_core_shapes` (src/node.c) is the same problem solved narrowly for
three known types; a general fix would subsume it.

## Required production completion

Architectural work with no shortcut, still outstanding:

- Replace the conservative source transformer with QuickJS parser-mode changes
  and shared parser tables for the LSP.
- Add the per-function ownership CFG and all SX bytecodes described by the ABI.
- Add frame-owned arena storage, exception-safe cleanup, object borrow locks,
  revocable interop proxies, and typed native registration to QuickJS.
- Replace the npm bootstrap delegation with the pinned native registry,
  integrity, extraction, resolver, lockfile, and trusted-hook implementation.
- Implement semantic LSP requests and parser-conformance sharing.
- The complete platform CI matrix.

What used to be listed here and now isn't: libuv-backed Node-compatible
timers, file, and fetch modules shipped (`spec/RUNTIME.md`, `spec/NODE.md`).
Remaining gaps in that surface -- `child_process`, `worker_threads`, generic
classes, decorators -- are tracked as feature gaps in those two documents
rather than as foundational work; they don't block anything else on this
list.
