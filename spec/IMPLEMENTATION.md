# Implementation ledger

## Implemented foundation

- Standalone CMake project and ArcSX QuickJS fork (originally a direct
  snapshot of Rayact's customized fork; see
  `third_party/ARCSX-PROVENANCE.md`).
- `.sx`, `.js`, `.mjs`, and `.cjs` CLI entrypoints.
- Native QuickJS parsing of `.sx` syntax -- interfaces, type annotations,
  `let mut`, `safe`, `unsafe`, and `&`/`&mut` borrow sigils -- with no
  separate transform step. The earlier in-memory text transformer
  (`sxfe_compile`, src/frontend.c) remains as an independently unit-tested
  component but is no longer on the execution path.
- One ownership rule is enforced rather than erased: `&mut x` requires `x` to
  be a `let mut` owner, and borrowing an immutable one is a parse error naming
  SX2003 (`js_parse_unary`, third_party/arcsx/quickjs.c). The check rules
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
- Fixed-layout structs are compiled to scalars. An `interface` whose members
  are all `i32`/`f32`/`f64`/`bool` is recorded per compilation unit while the
  declaration is still erased, so `: Name` classifies as `SX_TYPE_STRUCT`; a
  lexical binding of that type whose every use the compiler can account for is
  split into one local per field by `sx_scalarize_structs`, and no object is
  allocated. A use the pass cannot account for -- a capture, a borrow handed
  to a call, a reassignment, a read after the value moved, a literal that is
  not exactly the declared fields -- leaves that binding untouched, and a move
  builds the object at the point it escapes. The measured effect is below.
  `tests/fixtures/struct_sroa.sx` asserts the split is invisible, with every
  expected value taken from Node bar the one i32 wrap that is SX's by
  definition.
- Cycle sweeping when the event loop is quiet, plus `Sxn.gc()` and an `rss`
  field on `Sxn.memoryUsage()`. Why it was needed, and what it is worth, is
  below.
- `EventEmitter` warns once per emitter and event when a listener count
  crosses its limit, with Node's `MaxListenersExceededWarning` object and
  wording, and `setMaxListeners`/`getMaxListeners` to raise or disable it.
  `defaultMaxListeners` had been a constant nothing read.
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
  generated header; `third_party/ARCSX-PROVENANCE.md` has the lineage this
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

**It fired for one of the three ways to write the function.** Recorded
because the gap survived the fixture that was written to catch exactly this
kind of thing. The pass identifies a callee by `OP_fclosure` immediately
followed by `OP_put_loc`, and rejects a local touched by any other
`OP_FMT_loc` opcode. Only a hoisted `function` declaration produces that
shape: `resolve_variables` stores its closure straight into the slot at scope
entry (`OP_enter_scope`). Every other form is a function *expression* the
parser names after its binding, which puts an `OP_set_name` between the
closure and the store, and a lexical binding carries an
`OP_set_loc_uninitialized` for its dead zone besides. So

    function add(a: i32, b: i32): i32 { return a + b }   5.82 ns
    const  add = (a: i32, b: i32): i32 => a + b         17.58 ns
    var    add = function (a, b) { … }                  16.04 ns

on the M4, minimum of 9 runs over 5M iterations -- and the arrow is the form
`.sx` is written in. `tests/fixtures/typed_inline.sx` used declarations
throughout, which is why it never showed.

Both blockers are now matched: an optional `OP_set_name` between the closure
and the store, and `OP_set_loc_uninitialized` exempted from the
disqualifying-opcode test. The call site accepts a checked load as well as a
plain one, and when it was checked the load is kept and its value dropped
rather than erased, because a lexical callee can still be in its dead zone at
a call site that follows its store in bytecode order -- a `case` falling past
the declaration reaches one. That costs two opcodes against the frame's ten:
6.11 ns for the arrow against 5.80 for the declaration, both against 17.58
before. The fixture now covers all five binding forms and the dead zone, with
every expected value taken from Node.

Two limits remain worth stating rather than discovering later. The splice
requires the body to load every parameter once in declaration order, so
`(a, b) => a * b + c` and anything reusing a parameter (`v.x * v.x`) still
pays for its frame; lifting that needs the arguments in caller temporaries,
which means allocating locals after `resolve_variables`. And an inlined callee
no longer appears in a stack trace if its arithmetic throws -- the same
tradeoff every inlining compiler makes. A module-level function is a module
var rather than a local, so it is still never a candidate, which is what
keeps a real `.sx` module's own helpers out.

### The other large removable cost is the object, and the type removes it

The call frame was the first cost a declared type could delete outright. The
second is the object itself, and it is larger. A JIT reaches it by speculating
that a shape holds and deoptimizing when it does not; an interpreter cannot
speculate. What it can do is read a type that was written down.

`interface Vec3 { x: f64; y: f64; z: f64 }` is not a hint about an object. It
is an affine fixed-layout value (`spec/LANGUAGE.md`), with no identity anything
can observe, so a binding of that type that is never captured, borrowed or
moved is three numbers and the object is dead weight. `sx_scalarize_structs`
runs in the same pass-2 slot as `fuse_i32_accum_loops` and
`sx_inline_typed_calls`, matches every mention of such a binding against the
handful of byte sequences the parser emits for a field, and replaces the
binding with one local per field. Measured with
`benchmarks/engine/struct_sroa_probe.sx`, minimum of 9 runs over 2M
iterations, macOS arm64, Release, per loop iteration:

| | ns/op |
|---|---|
| empty loop | 4.07 |
| create + update: untyped | 80.60 |
| create + update: typed | 11.19 |
| create + update: hand-split | 12.49 |
| update only: untyped | 18.31 |
| update only: typed | 6.40 |
| update only: hand-split | 6.39 |

80.6 to 11.2 ns on the row that allocates, 7.2x, and 18.3 to 6.4 on the row
that only reads and writes fields, 2.9x. Both land on the hand-written
ceiling -- the same arithmetic over three separate `let`s -- which is the
bound this can reach, and the create row edges past it because the three
`let`s of the hand-written version are three separate declarations. The
`benchmarks/wintertc` rows are unmoved (buffer 19.9 ms, textencoder 4.9,
events 6.8, worst pause 0.02), which is expected: no workload there declares
a struct.

Read the first row against the ledger's own object numbers rather than against
a JIT. An empty `{}` costs 76 ns to create and destroy here and a two-field
literal 79 ns even with the shape keep-alive ring, and the section below
records that reaching a few nanoseconds per object means not doing per-object
work at all -- bump allocation and a nursery, which is a different engine.
This does something else: it does not make the object cheaper, it removes the
object. The nursery argument is untouched by it, and so is the object-literal
shape churn below, which is still there for every literal the compiler cannot
account for.

Four limits are worth stating rather than discovering.

The binding must be lexical. A `var` is hoisted out of the block it is written
in, so a `var` whose initializer throws halfway is still readable afterwards,
and it would then read the fields written before the throw rather than the
`undefined` the binding actually holds. A `let` cannot be reached that way: an
initializer that throws leaves its own block, and everything that could
observe the binding goes with it.

A move must be the last mention of the binding in bytecode order, and must not
sit inside a loop. Moving a struct into JavaScript boxes a copy, which is the
defined semantics, but the ownership rules that make a move final are not
enforced yet -- so a program that violates them by using the binding after
moving it must not be able to tell that it did. Those two conditions are what
a control-flow pass would replace with SX2001.

A borrow keeps the object. `&mut v` hands a callee something it may write
through, so the pass gives up on that binding entirely rather than trying to
write the callee's mutations back into the caller's locals. Lifting that is
the same work as extending `sx_inline_typed_calls` to a struct parameter, and
wants doing in one piece: an inlined callee reading `v.x` from the caller's
locals is where the borrow rules pay, and 28.9 -> 18.1 ns is the hand-inlined
bound recorded above for a call that still keeps its object.

The field must be named statically. `p[k]`, a `delete`, an `in`, and a literal
whose fields are not exactly the declared ones all keep the object, because
each of them asks a question only a property table can answer.

One latent defect turned up on the way, and is fixed here rather than left:
`OP_inc_loc_safe_i32` declared size 1 in `quickjs-opcode.h` while its
interpreter case reads a one-byte operand, so `opcode_info` disagreed with the
emitter by a byte. Nothing emitted it -- it is selected only for an unchecked
`put_loc`, and every binding that can select it is lexical and therefore
checked -- so it had never mattered, and briefly making the path live is what
surfaced it. It stays dead; the declaration is now honest.

### The property cache was flushed by every property anyone added

`js_prop_cache_invalidate` bumps a runtime-wide generation counter, which
invalidates every entry in the shape-keyed property cache at once. It was
called unconditionally on entry to `add_property` -- so every object literal
field, every constructor field and every expando threw away the whole cache.
An ExpressX request builds a 13-property response, adds eight properties to a
native `Request` and builds an outcome object, so it did that twenty-odd
times, and every field read between them paid a miss and a refill.

Only a prototype's new property can make a live entry wrong. The cache answers
"a receiver of shape S finds this at depth d, slot k", and it is only ever
filled from a property that was *found*. Adding to an ordinary object changes
that object's own shape, so an entry keyed on the old one stops matching it
and nothing else moved. Adding to a prototype can shadow something a site
already found further down the chain, and that is the one case where a live
entry is still matched and is wrong. Every other way a property can move --
resize, compact, in-place shape update, prototype reassignment, a freed
shape's address -- already invalidates at its own site.

That gate needed one other change to be sound. `is_prototype` was set only in
`JS_SetPrototypeInternal`, which leaves it false for `Foo.prototype` and for
every builtin prototype: upstream uses the flag only to track
`Array.prototype`, not to answer "is anyone inheriting from me". It is now set
in `js_new_shape_nohash`, the single funnel every shape creation passes
through, so an object is flagged the moment a shape names it as a prototype --
which necessarily precedes any cached entry whose chain includes it, because
an entry is only filled from a receiver that was actually walked.

`tests/fixtures/prop_cache_shadow.mjs` is the guard: warm call sites, then own
properties shadowing inherited ones, a nearer prototype shadowing a further
one, `Object.prototype` gaining and losing a property, a constructor
prototype's method replaced and then shadowed, `setPrototypeOf` under a warm
site, an accessor redefined as data, and 500 iterations of ordinary object
churn that must disturb none of it. Node runs the same file and agrees.
Removing the `is_prototype` line makes it fail on the `Foo.prototype` case,
which is the one upstream leaves unflagged.

Worth, measured three ways:

| | flush every add | gated |
|---|---|---|
| 12 warm reads beside one 4-field literal | 150.9 ns | 128.0 |
| ExpressX, per request, seven shapes | -- | 0.1 to 1.8% |
| `benchmarks/wintertc` throughput | -- | unmoved |

The first row is the shape the cache exists for and the interference is
mostly gone: 25.7 ns of it per iteration against 5.6. The other two are the
honest part. ExpressX reads few fields twice on the same shape -- a fresh
`Request` and a fresh response object per request -- so most of its reads
would miss anyway, and the wintertc workloads are Buffer, TextEncoder,
EventEmitter and JSON, none of which is a warm read set. The change is kept
because it is strictly less work for the same answer, not because those two
rows moved.

### A JIT is ruled out by platform, not by effort

iOS does not grant W^X/JIT entitlements to third-party apps, so a
machine-code tier would make this runtime unusable on a target platform.
That rules out the one technique -- generating machine code -- that a JIT
uses to close this gap. It does not rule out closing it some other way; the
dispatch floor above is the floor of what's been tried, not a proof that
nothing faster exists. Benchmarks against JIT runtimes should be read with
that in mind: on JIT-bound microbenchmarks the comparison is against a
technique this project won't use, not necessarily a result it can't reach.

### An idle process never collected, and the leak protected itself

This one is a correctness result rather than a speed one, and it is the
counterpart to the two negative results below: the collector's *cadence*
turned out to be worth far more than its *mechanism*.

Before this change, nothing in `src/` ever called `JS_RunGC`. The only
GC-related line in the whole project was `JS_SetGCThreshold(runtime, 8 MB)` in
`src/main.c`. Collection happened solely inside `js_trigger_gc`, when an
allocation would cross the threshold -- and that function then sets the next
threshold to `malloc_size + (malloc_size >> 1)`, 1.5x whatever was live.

Those two facts combine badly. Measured with `Sxn.memoryUsage()`, building
200,000 objects that each hold `self` and a closure reaching back into them,
holding the batch, then dropping it:

| | tracked bytes | collections |
|---|---|---|
| start | 734,672 | 0 |
| after the same burst built **acyclic** | 735,584 | 8 |
| after the **cyclic** burst is dropped | 229,535,648 | 9 |
| after 12 further rounds of ~8 MB churn | 246,984,720 | **9** |

Peak RSS 251 MB. The last two rows are the finding: 96 MB of subsequent
allocation and release produced *zero* collections. The collection at the peak
correctly sized the threshold for a 229 MB live set; the burst then died, but
nothing re-evaluates a threshold except another collection, and the later
churn is freed by refcounting as it goes so the bar is never reached again.
**The larger the cyclic garbage, the higher the bar for collecting it**, and
each burst raises a floor that never comes down.

Three things it is *not*, each checked rather than assumed. Acyclic garbage is
freed immediately by refcounting (row two). Cycles under sustained load are
collected fine -- 500,000 cyclic closures in a tight loop fired 708
collections and finished at 743 KB against a 736 KB baseline, and a
3,000-request `Sxn.serve` run creating a cycle per request stayed at 787 KB.
And the connection and callback registries do not retain: `ConnState` is a C
list unlinked on close and is deliberately passed through its promise
continuations *as an integer* rather than a JS value, `SxnTimer` is unlinked
in `sxn_timer_stop`, and the fetch, chunk-view and UDP states each hold their
callbacks in a C struct with a matching finalizer. Only quiescence after a
peak leaks.

The fix is `sxn_maybe_idle_gc`, called from both loops in `src/network.c` --
`sxn_run_event_loop` and `sxn_await_with_loop`, because a module with
top-level await never reaches the first, and hooking only it left every
`await` daemon uncollected. It sweeps when the previous `uv_run` blocked for
20 ms or more, tracked size is above a 32 MB floor, and half a second has
passed since the last sweep. It then resets the threshold, which is the
load-bearing half: `JS_RunGC` reclaims without touching it, so a sweep that
does not also reset leaves the ratchet in place for the next burst.

The first attempt gated on tracked size not growing between turns and never
fired at all -- any loop with a timer on it allocates a trickle every turn, so
the size always crept up. Timing how long `uv_run` blocked is the direct
measurement and is what shipped.

Results. A burst-then-idle daemon now reclaims 171 MB across one loop turn
with nothing calling anything (`tests/fixtures/leak_idle_sweep.mjs`), and
`Sxn.gc()` takes 229.5 MB back to 735 KB explicitly. A server under sustained
load takes *no* sweep at all -- 4,000 requests, zero collections, and the same
wall time with the feature on and off -- because `uv_run` never blocks long
enough to open the gate. The benchmark rows are unmoved: buffer 18.9 ms,
textencoder 4.8, events 6.5, worst pause 0.04 ms, all matching the figures
above and matching a `--no-idle-gc` run.

`benchmarks/engine/gc_idle_probe.sx` re-derives the whole thing and reports
RSS beside tracked bytes. Note the two diverge exactly as expected: after a
sweep that took tracked bytes from 164 MB to 0.7 MB, RSS only moved 199.8 ->
196.8 MB, because the system allocator kept the pages. That is why the
fixtures assert on `mallocSize` and only report `rss`.

Two items proposed alongside this were not built. Reworking the network
registries onto `WeakRef` has nothing to attach to, per the audit above.
Statically flagging non-escaping values to skip cycle-collector registration
is the `-DSXN_ABLATE_GC_LIST` experiment below, whose exact upper bound is
0-1 ns -- and unregistering a value that can still enter a cycle is how a
collector frees something reachable, so the risk is not proportionate to a
measured zero.

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
  The CFG is what would replace the two conservative conditions
  `sx_scalarize_structs` currently puts on a move, and what SX2001 needs. The
  allocation bytecode is deferred by measurement rather than outstanding: a
  struct the compiler can account for is never allocated at all, and one that
  escapes is built by the ordinary object opcodes. `spec/ABI.md` records that.
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
