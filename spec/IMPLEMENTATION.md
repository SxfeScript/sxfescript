# Implementation ledger

## Implemented foundation

- Standalone CMake project and direct Rayact QuickJS snapshot.
- `.sx`, `.js`, `.mjs`, and `.cjs` CLI entrypoints.
- In-memory `.sx` transformation for interfaces, annotations, `let mut`, borrows,
  and `unsafe`, with stable unsupported-syntax diagnostics.
- Fixed-layout calculation and aligned growable/poisonable arena primitives.
- Module-loader hook that transforms imported `.sx` modules in memory.
- Package command surface with safe argument validation, disabled lifecycle
  scripts, and a bootstrap npm-compatible backend.
- LSP JSON-RPC transport and VS Code language registration.

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

