# SxfeScript and SXN

SXN is a standalone QuickJS-based runtime for `.sx` systems code and ordinary
JavaScript. SxfeScript adds explicit mutation, affine values, borrows, and
erasable TypeScript-style annotations without a Vite or AOT build step.

This repository is intentionally independent from Rayact. Its QuickJS source is
a direct snapshot of Rayact's customized fork at commit `66f4965`.

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
