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
