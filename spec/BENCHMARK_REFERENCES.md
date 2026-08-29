# High-performance JavaScript benchmark references

This is the reference ledger for the comparisons in `README.md`. The goal is
not to transplant JavaScript source into ArcSX, but to identify the operation
that makes a JavaScript implementation fast and move that operation into the
native QuickJS layer when its semantics permit.

## Buffer

* [feross/buffer](https://github.com/feross/buffer) is the compatibility
  reference. It deliberately uses `Uint8Array`/`ArrayBuffer` backing and
  changes the returned view's prototype; its JavaScript conversion loops are
  useful correctness fallbacks, not the performance ceiling.
* [hextreme](https://github.com/jawj/hextreme) is the strongest JavaScript
  conversion reference found. Its hex encoder builds 256-entry 16-bit lookup
  tables and processes four input bytes at a time through `Uint32Array` views.
  The equivalent native C path already uses a compile-time 256-entry pair
  table and direct two-byte stores; a future SIMD pass can be measured against
  this, but it is not needed for the current short-string benchmark.
* [Node's native Buffer implementation](https://github.com/nodejs/node/blob/main/src/node_buffer.cc)
  and [string byte codecs](https://github.com/nodejs/node/blob/main/src/string_bytes.cc)
  are the authoritative native design references: validate the view once,
  obtain its backing pointer, size the output, and encode directly into the
  final allocation.

ArcSX has applied the transferable parts: one-pass UTF-8 into the final
ArrayBuffer, zero-copy Buffer views, pinned typed-array shapes, atom-based
encoding dispatch, and direct native hex conversion.

## TextEncoder

* [FastestSmallestTextEncoderDecoder](https://github.com/anonyco/FastestSmallestTextEncoderDecoder)
  is the best pure-JavaScript reference found for UTF-8. Its benchmark covers
  both tiny and large strings, checks correctness, and shows that allocation
  and engine call overhead matter as much as the code-point loop.
* [fast-text-encoding](https://github.com/samthor/fast-text-encoding) is a
  useful tiny-string comparison, but is slower on larger mixed-Unicode input
  in the published tests.

The useful algorithmic ideas—pre-size once, combine surrogate pairs in the
same pass, and never split a code point in `encodeInto`—are already native in
`src/network.c`. The remaining gap to Bun is the cost of creating a fresh,
mutable `Uint8Array` for every `encode()` result; returning a shared result or
pooling it would violate Web API mutation semantics without a copy-on-write
typed-array implementation.

## EventEmitter

* [tseep](https://github.com/Morglod/tseep) is the strongest current
  JavaScript reference. It stores one listener as a function, promotes to an
  array for multiple listeners, and uses generated fixed-arity dispatch for
  its largest JIT win.
* [EventEmitter3](https://github.com/primus/eventemitter3) is the conservative
  compatibility/performance reference, with extensive add/remove/emit
  benchmarks and no required code generation.

ArcSX now uses the function-for-one/array-for-many representation natively,
including promotion, demotion, introspection, and mutation-safe emit. The
generated/eval dispatcher is not a valid equivalent for this runtime: it adds
an extra interpreted call under QuickJS, and the large published gain depends
on a JIT compiling the generated function. The remaining benchmark cost is
executing the user listener bytecode itself.

## Allocation and pause consistency

There is no semantically equivalent “fast JavaScript implementation” of an
allocation benchmark: replacing `{}` with a pool changes object identity,
finalization timing, and observable allocation behavior. JavaScript engines
win this class with generational nurseries and optimized allocation stubs.
The native equivalent available without a JIT is ArcSX's per-size arena and
spare-arena recycling, which reduces allocator cost while preserving each
object's identity and lifetime.

## Parse 32k-line generated file

* [Meriyah's speed comparison](https://meriyah.github.io/meriyah/performance/)
  is the best pure-JavaScript parser reference found.
* [Acorn's parser benchmark](https://marijnhaverbeke.nl/acorn/test/bench/index.html)
  is a useful independently maintained baseline.

Those parsers accept ECMAScript and produce a general AST. This row is a
generated SX/JavaScript declaration stress test, so replacing the frontend
with one of them would change the language contract. The transferable lesson
is linear symbol indexing; ArcSX's resolver now uses indexed declaration and
closure lookups and already wins this row.

## Cold start and real-world task

These are composite/process benchmarks, not library benchmarks. The useful
native translations are startup footprint and direct system primitives:
`Sxn.serve`, native fetch, environment access, and the already-native Buffer,
TextEncoder, EventEmitter, and path hot paths. A JavaScript package cannot
remove the interpreter process launch or replace the OS/network work without
changing what is measured.

