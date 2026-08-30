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

## Performance shape (measured, benchmarks/wintercg/run.sh)

- sxn wins: process cold start (~0.01s vs node ~0.04-0.07s), worst-case
  pause consistency under sustained allocation (0.05ms vs 0.21ms worst
  single pause over 2M allocations), and `safe`-kernel scalar loops
  (native loop fusion beats node outright at any iteration count).
- node/V8 wins: sustained throughput on allocation- and call-heavy loops
  (2x-30x depending on workload) because V8 JIT-compiles hot code and
  QuickJS interprets. The floor is intrinsic: a bare indirect function
  call in a loop is ~18x slower interpreted than JIT-inlined. Closing it
  in general requires a JIT tier, tracked as future work, not claimed.

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

### A JIT is ruled out by platform, not by effort

iOS does not grant W^X/JIT entitlements to third-party apps, so a
machine-code tier would make this runtime unusable on a target platform.
That makes the dispatch floor above a permanent design property rather than
a gap to be closed, and it rules out the single technique that would close
it. Benchmarks against JIT runtimes should be read with that in mind: on
JIT-bound microbenchmarks the comparison is against something this project
structurally cannot do.

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
`benchmarks/wintercg/pause.sx`, where every allocation dies immediately --
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

## Known: parsing is still quadratic in declarations per scope

Parsing a file with many declarations in one scope is O(N^2). A 32k-line
file of top-level `let`s takes ~0.73 s against Node's 0.06 s, and the cost
grows quadratically -- large bundles and generated code pay it, and it
undercuts the cold-start advantage that is this runtime's main strength.

The cause is a set of linear scans, each run once per declaration or
reference. Two on the *parse* path are fixed (`find_var_in_child_scope` and
`find_global_var`, the latter given the same open-addressed index that
`find_var` already had), worth ~29%: 1.03 -> 0.73 s at 32k lines.

The remaining one is in the *resolution* pass, and is located precisely:
`resolve_scope_var` walks `s->scopes[scope_level].first` down `vd->scope_next`
to match a name. The chain is ordered most-recent-first, so at parse time a
declaration finds itself immediately -- but `resolve_variables` runs later,
over the whole function, when the chain holds every variable in scope. A
reference to an early variable then walks the entire chain, giving O(N/2)
per reference and O(N^2) overall. A 60k-declaration file profiles as
`resolve_scope_var` 1086 samples and `resolve_variables` 721, with
`define_var` no longer significant.

Fixing it needs a real index rather than the negative-filter trick used on
the parse path, because the lookup must return *which* variable matches, not
merely whether one exists: an index keyed by (scope chain, name), or a
per-name chain of variable indices that resolution can search in scope
order. Redeclaration in a single scope is already an error, so a name maps
to at most one variable per scope level -- the difficulty is only that the
chain spans enclosing scopes.

Reproducer: `benchmarks/engine/parse_scale.js` (32k declarations); generate
other sizes to confirm the curve. Any fix must keep
`tests/fixtures/declaration_scoping.mjs` passing -- redeclaration errors,
block shadowing, TDZ and per-iteration closure capture all depend on these
lookups being exact.

## Required production completion

- Replace the conservative source transformer with QuickJS parser-mode changes
  and shared parser tables for the LSP.
- Add the per-function ownership CFG and all SX bytecodes described by the ABI.
- Add frame-owned arena storage, exception-safe cleanup, object borrow locks,
  revocable interop proxies, and typed native registration to QuickJS.
- Replace the npm bootstrap delegation with the pinned native registry,
  integrity, extraction, resolver, lockfile, and trusted-hook implementation.
- Implement semantic LSP requests and parser-conformance sharing.
- Add libuv-backed Node-compatible timers/files/fetch modules and the complete
  platform CI matrix.
