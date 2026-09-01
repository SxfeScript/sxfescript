# SXN command contract

- `sxn file.sx|ts|js|mjs|cjs|sxbc [args...]` executes a file directly.
- `sxn [--memory-report] [--leak-check] [--compile-cache] <file> [args...]`
  runs a file with diagnostics on, or (`--compile-cache`) via a bytecode
  cache built and reused across launches -- see `spec/BYTECODE.md`.
- `sxn [--no-idle-gc] [--idle-gc-floor=<MB>] <file> [args...]` tunes cycle
  sweeping while the event loop is quiet. A long-running server that takes a
  burst leaving reference cycles behind and then waits would otherwise hold
  them: the collector runs only on allocation, and the collection at the
  burst's peak has already raised its threshold above what leaked. So the loop
  sweeps when it has been blocked waiting, has not swept in the last half
  second, and is holding more than the floor -- 32 MB by default, below which
  nothing is swept and a small server keeps exactly the pause profile it had.
  `--no-idle-gc` turns it off entirely. `Sxn.gc()` is the explicit ask,
  independent of both.
- `sxn compile <file> [-o out.sxbc] [--strip]` compiles a file to bytecode
  for distribution, without running it. `spec/BYTECODE.md`.
- `sxn run [script] -- [args...]` executes a `package.json` script.
- `sxn install` installs dependencies without lifecycle scripts.
- `sxn add [--dev] package[@range]` adds and installs a dependency.
- `sxn remove package` removes a dependency.
- `sxn init` creates a minimal package.
- `sxn lsp --stdio` runs the language server transport.

Extensionless resolution order (an import or require with no extension, or a
bare directory) is `.sx`, `.mjs`, `.js`, `.cjs`, `.json`, `.node`, `.ts`, then
the same list again under `index.*` for a directory. Lifecycle hooks are
disabled unless their package is explicitly named in the top-level
`trustedDependencies` array.
