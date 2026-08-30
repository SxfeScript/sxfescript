# SXN command contract

- `sxn file.sx|ts|js|mjs|cjs|sxbc [args...]` executes a file directly.
- `sxn [--memory-report] [--leak-check] [--compile-cache] <file> [args...]`
  runs a file with diagnostics on, or (`--compile-cache`) via a bytecode
  cache built and reused across launches -- see `spec/BYTECODE.md`.
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
