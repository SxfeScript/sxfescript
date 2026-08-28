# SXN command contract

- `sxn file.sx|js|mjs|cjs [args...]` executes a module.
- `sxn run [script] -- [args...]` executes a `package.json` script.
- `sxn install` installs dependencies without lifecycle scripts.
- `sxn add [--dev] package[@range]` adds and installs a dependency.
- `sxn remove package` removes a dependency.
- `sxn init` creates a minimal package.
- `sxn lsp --stdio` runs the language server transport.

Extensionless resolution order is `.sx`, `.mjs`, `.js`, `.cjs`, `.json`.
Lifecycle hooks are disabled unless their package is explicitly named in the
top-level `trustedDependencies` array.

