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
- Primitive `unsafe extern` declaration lowering to the explicit `Sxn.ffi`
  boundary exists in the standalone compatibility transformer, but native
  parsing does not implement the FFI codegen yet -- it rejects `extern`
  declarations with a clear "not yet supported" error rather than
  mis-parsing them; dynamic loading and libffi trampolines remain pending.
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

Two things were ruled out by measurement along the way, and should not be
retried without new evidence:

- **Allocation count is not the limiting factor.** `encodeInto`, which
  allocates nothing, measures *slower* (130.6 ns) than `encode`, which
  allocates a fresh array (101.3 ns). Reducing `new Uint8Array(40)` from 7
  allocations to 5 moved the TextEncoder benchmark by ~1 ms.
- **The object model is not the gap either.** `new Plain()` (58.7 ns) and a
  bare `{}` (58.3 ns) cost the same, so constructor and prototype machinery
  is not what is being paid for; eliding `.prototype` resolution would gain
  approximately nothing, and memoizing its slot measured ~2%.

What remains is a JIT tier, plus a generational nursery for object churn
(freeing a small object costs ~29 ns here against Bun's ~1 ns, which is
refcounting versus a nursery that reclaims dead young objects for free).
The nursery is not a free win to adopt: the same refcounting that makes
teardown expensive is what produces this runtime's 0.05 ms worst-case pause
against Bun's 2.62 ms, which is its strongest measured property.

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
