# Using node: packages

Point `sxn` at a Node project and it usually just runs. CommonJS and ESM both
work, `node_modules` resolves the way Node resolves it, the `node:` builtins
are there, and `.node` native addons load over 120 Node-API entry points.

```sh
sxn ./node_modules/.bin/some-cli
sxn server.js
```

## Module resolution

`sxn` decides module-or-CommonJS the way Node does. `.mjs` and `.mts` are
always modules, `.cjs` is always CommonJS, and a plain `.js` file — or an
extensionless one, which is what every npm CLI ships — follows the nearest
`package.json`'s `"type"`, defaulting to CommonJS. A `#!/usr/bin/env node`
shebang is stripped before evaluation, so those CLIs run directly.

Extensionless imports resolve in the order `.sx`, `.mjs`, `.js`, `.cjs`,
`.json`, `.node`, `.ts`, then the same list again under `index.*` for a
directory.

```js
const express = require("express");
import { readFile } from "node:fs/promises";
```

Both forms work in the file type that allows them, and a `.sx` module can
import either.

## What is there

A superset: every `node:` builtin, plus the WinterTC web APIs Node only has
part of. The [Node compatibility reference](../node/) is specific about each
`node:` module — what is implemented, what is native C and what is
JavaScript, and where a gap is deliberate rather than pending.

The headline gaps, so you can check them first:

- **`child_process`** — `spawn`, `exec`, `execFile` and every `Sync` form work
  over `uv_spawn`. The asynchronous forms run the child to completion on a
  loop of their own, so output arrives in one piece at the end rather than as
  it is produced, and `fork` throws: a child would need a second runtime.
- **`worker_threads`** — not implemented.
- **Native addons** — `.node` files load, and the Node-API implementation is
  real enough that `next-swc`, the Rust binary Next.js compiles JSX with, runs
  under it.

## Native addons

A `require("./thing.node")` resolves through the same Node-API surface Node
exposes, implemented from scratch against the published headers rather than
wrapped around V8. [Calling C](../native/) explains why addon loading and
`Sxn.ffi` are two different things, and which of them belongs to the engine.

## Packages

`sxn install`, `sxn add`, `sxn remove` and `sxn init` cover the package
workflow. Lifecycle scripts are disabled unless the package is named
explicitly in a top-level `trustedDependencies` array — an install should not
be able to run arbitrary code because a transitive dependency asked to.

```sh
sxn install
sxn add --dev typescript
```

## What to read next

- [Node compatibility reference](../node/) — the per-module detail.
- [The runtime surface](../runtime/) — the web APIs that exist alongside the
  Node ones.
- [The CLI](../cli/) — every command and flag.
