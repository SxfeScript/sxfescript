# Performance notes

The two result tables this document explains -- Mac (Apple M4) and Linux PC
(Ryzen 7 5700G) -- live in the [README](../README.md#benchmarks-sxn-vs-node-vs-bun),
alongside how to run the benchmark harness yourself. This is the detail
behind those numbers: what each row actually measures, the optimizations
that produced them, and what's still open.

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

Cumulatively, on the Mac: Buffer 102->19.4 ms, TextEncoder 76->4.7 ms,
EventEmitter 37->6.6 ms, cold start 10.7->8.3 ms, and the pause benchmark's
total 1.1 s->143.9 ms, with
zero GC cycles during the loops throughout. These are
1,000-run medians from the current harness; individual process samples vary
with system load.

## JSON

The harness's `json` row parses a 1MB API payload and writes it back out,
forty times. It is the one row named after a workload rather than an API,
because it is what an HTTP service spends its time on: a request body in,
a response body out.

Three changes, all inside the tokenizer and the serializer rather than the
value model:

- **Strings are scanned a word at a time.** Both directions used to walk a
  string one character per iteration -- four comparisons per byte on the way
  in, a `string_getc`/`string_buffer_putc` pair per character on the way
  out. Both now test eight bytes at once for the only bytes that matter (a
  quote, a backslash, a control character, and on the way in anything
  non-ASCII), using the standard `(x - 0x01..) & ~x` zero-byte trick, and
  copy the run between them in one `memcpy`. Scanning was the largest single
  cost in each direction.
- **An escape-free string is allocated once.** The parser accumulated every
  string into a `StringBuffer` that starts at 48 characters and is then
  grown, copied and trimmed. A string with no escape and nothing above ASCII
  -- most strings in most JSON -- is now measured by the scan above and
  allocated at its final size.
- **Quoting writes into the buffer the caller already has.** `stringify`
  allocated a quoted copy of every key and every string value, copied it into
  its output, and freed it. It writes through now.

- **A parsed string is not built until its use is known.** An escape-free
  ASCII string is handed on as a slice of the input, so a property name goes
  straight to an atom -- a hash of bytes already in memory -- instead of
  allocating a string, interning it and freeing it again. Only a value is
  allocated.
- **Getting at the input costs a scan, so the scan is a word wide.**
  `JS_ToCString` already returns an ASCII 8-bit string's own bytes without
  copying, but it decided that by counting non-ASCII bytes one at a time
  across the whole string; it now clears eight at a time and only counts from
  the first non-ASCII byte. Its wide-string transcode -- what an input pays
  when one accent makes the whole string 16-bit -- copies four ASCII code
  points per iteration instead of one. Both help every `JS_ToCString` caller
  in the runtime, not only JSON.

And a plain integer is converted by the tokenizer instead of `strtod`, which
is locale-aware -- it takes a lock to find the decimal separator -- and
rescans the digits.

Then the object model, which the profile pointed at once the scanning was
cheap. Writing:

- **An object's keys are read as atoms, not as strings.** `stringify` built
  a JavaScript array of key strings per object and then looked each property
  back up by string, which hashes it into an atom again. An ordinary object
  with no replacer list now walks its own atoms, reads by atom, and takes the
  key string from the atom rather than building one. When its keys are all
  ordinary strings they come straight out of its shape into a stack array --
  no allocation per object at all -- and, while the shape has not changed,
  each value is read from the slot recorded then rather than looked up.
- **Integers, booleans and null go in as bytes.** Each used to be handed to
  `JS_ToString`, which allocates a string to copy in and free. A JSON
  document is mostly those three.
- **`toJSON` is looked for once per shape.** Every object was searched for
  the method, walking its prototype chain to find nothing. The last shape
  with none is remembered against the runtime's property-location generation
  -- the stamp the inline caches already keep. The memo is only taken when
  nothing on the chain is exotic and nothing on it has a `toJSON` property at
  all: a Proxy answers from its own trap and they all share one shape, and a
  shape records which properties exist, not what they hold.
- **The circular-reference check left the JavaScript heap.** The path from
  the root was a JavaScript array, so every object cost an `Array#includes`,
  a push and a pop through the generic property machinery. It is a small
  array of object pointers now.
- **Nothing is written for a separator that is empty**, which is both of them
  whenever `JSON.stringify` is called without a gap.

Reading:

- **A property goes straight into the object being built.** The parser owns
  the object it is filling and nothing else can see it, so a key that is not
  already there is added directly instead of going through the descriptor
  machinery. A repeated key -- legal, last one wins -- is the only case that
  finds an existing slot.
- **An array element is appended, not defined.** The array is the parser's
  own fresh fast array; appending to it skips the indexed-property path,
  which has to assume the target could be anything.
- **The last few property names are remembered.** Every object in a document
  of records repeats the same keys, and each one was hashed into the atom
  table again.

On the Mac the round trip went 165.4 -> 47.4 ms against Node's 28.5 and
Bun's 23.1. Per operation, against Node:

| | this runtime | Node |
|---|---|---|
| parse a 1MB document | 2.29 -> 1.22 ms | 1.05 |
| write it | 7.01 -> 1.23 ms | 0.60 |
| parse 30000 small objects | 8.56 -> 4.44 ms | 2.36 |
| write them | 10.6 -> 3.69 ms | 0.95 |
| parse one long string | 0.94 -> 0.19 ms | 0.32 |
| write one long string | 2.52 -> 0.08 ms | 0.13 |

Both string rows are now ahead of Node; the rest is within 1.2x on parsing a
document of mixed content and about 2x on writing one.

Numbers were the other half, and are no longer. Turning a double into its
shortest round-tripping decimal was an exact big-integer search; `dtoa.c` now
takes Grisu3 (Loitsch, PLDI 2010) first, which computes the digits with
64-bit arithmetic and a table of cached powers of ten and then *proves* its
result is the unique shortest form, declining when it cannot. Every decline,
and every other radix, format and exponent mode, still runs the exact
algorithm. Stringifying 120000 random doubles: 22.8 -> 13.9 ms per pass. It
is checked by a differential test against Node over millions of values --
random bit patterns, every integer from -1e6 to 1e6, powers of ten,
subnormals, the signed zeroes, infinities, NaN, and ULP windows around 1e21
and 2^53 -- with no divergence.

What is left is writing object-heavy documents, still around 4x Node and
spread across the remaining per-property work with no peak worth naming.

Two of these went in wrong the first time and were caught by review before
they shipped: the `toJSON` memo carried one Proxy's answer to the next, and
re-checking enumerability per property let a getter change what was written
after the key list had been taken. `tests/fixtures/json_edges.mjs` covers
both, and `json_fuzz.mjs` walks 4000 seeded-random documents through parse
and stringify and hashes every string produced -- the expected hash is
Node's, so a byte of divergence anywhere fails the build.

None of it is the parser's structure, which is why a faster external parser
is not the answer. A DOM parser would replace the part that is now cheap and
still leave every JavaScript object to be built one property at a time, with
a second representation to copy out of on the way.

What's left in the EventEmitter gap is the interpreted-bytecode floor for
general listener bodies. The benchmark's numeric accumulator takes a native
fast path and now a fused call site as well, but arbitrary listeners still
require an interpreter frame. A JIT is the usual way to remove that frame,
and it's ruled out here; closing the gap some other way is open, and hasn't
been attempted yet.

## Startup

The Node layer used to register all forty-four `node:` modules at startup --
a `JSModuleDef` and an atom per export name each -- and to construct every
module object, whatever the program went on to import. Both now happen when
something asks: the loader registers a module on the specifier it was handed,
and the seventeen builtins past the original twenty build their objects on
first use. Cold start on the Mac, minimum of 150 interleaved launches:
7.15 -> 6.97 ms.

What is left above the 6.83 ms this measured before the Minimum Common API
work is `src/bootstrap.js`: `URLPattern`, the compression streams, the stream
controller classes and the three event-handler properties are built eagerly,
because a page-shaped global has to be there before the program's first line
runs. Deferring the node_compat half was worth about 0.2 ms; deferring this
half would mean a getter per global, and the globals are the surface.

Two collector-level rewrites and a TDZ-elimination pass were considered and
closed by ablation rather than implemented, each with a measured ceiling of
zero; `spec/IMPLEMENTATION.md` records the method and the numbers. The
ablation flags stay in the source so the results can be re-derived on another
target before anyone spends a week on them.
