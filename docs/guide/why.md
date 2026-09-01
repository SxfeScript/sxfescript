# Why sxn

`sxn` is a JavaScript runtime. It runs your `.js`, `.mjs`, `.cjs` and `.ts`
files, loads packages from `node_modules`, implements the `node:` builtins, and
serves HTTP. If you have a Node script, `sxn script.js` is usually all you need
to try it.

Two things make it different from the other runtimes you could pick.

**It starts fast and stays even.** Cold start is 8.4 ms against Node's 41.6 on
the same machine. The worst single garbage-collection pause across a churning
workload is 0.04 ms against Node's 0.36 and Bun's 2.59. There is no JIT to warm
up, so the first request costs what the thousandth does.

**It has no JIT on purpose.** iOS will not grant a third-party app the
entitlement to generate machine code. A runtime that needs a JIT to be fast
cannot run there at all, so this one is built to be fast without one — which
means the same binary behaves the same on a phone, a laptop and a server.

That tradeoff is real and it cuts both ways. On a tight numeric loop, a JIT
compiles the loop away and this runtime cannot. The benchmarks below are the
honest version: seven of eight categories, and the eighth is Node's.

## Against Node and Bun

Apple M4, same tree, same tests. Lower is better in every row.

<!-- include-section: README.md#mac-apple-m4 -->

The [benchmarks page](../benchmarks/) has the second machine, both machines'
specs, and how each row is measured.

## What you get in the box

| | sxn | Node | Bun |
|---|---|---|---|
| Run `.ts` with no build step | yes | type stripping only | yes |
| Ownership and borrows in the language | yes | no | no |
| Single binary, no runtime deps to install | yes | yes | yes |
| Standard library | [superset](../node/) | **the reference** | most of it |
| `.node` native addons | [120 Node-API entry points](../native/) | yes | partial |
| WinterTC Minimum Common API | 62 of 62, less WebAssembly | partial | most |
| Call a C function without an addon | `Sxn.ffi` | no | `bun:ffi` |
| Precompile to skip parsing | `.sxbc`, still needs `sxn` installed | no | no |
| Standalone binary, no runtime to install | no | experimental (SEA) | `bun build --compile` |
| JIT | no, deliberately | yes | yes |
| Runs on iOS | yes | no | no |

"Superset" describes the surface, and Node still wins that row. `sxn` has the
`node:` builtins *and* the whole WinterTC Minimum Common API, which Node only
partly has — but Node is the reference implementation and two of its modules
are not fully here. `worker_threads` is not implemented, and `child_process`'s
asynchronous forms run the child to completion and hand you its output in one
piece rather than streaming it. The [per-module list](../node/) is specific
about every module.

## When to pick something else

Worth saying plainly, because a comparison that never loses is not a
comparison.

- **A JIT-bound workload.** If your hot path is a numeric loop that a JIT can
  compile away, Node and Bun will win it and the gap will be large. The
  [performance notes](../performance/) quote the floor: an interpreter pays
  about 16 ns per opcode dispatch, and a JIT pays roughly nothing.
- **The widest package compatibility.** Node is the reference implementation.
  `sxn` implements the `node:` surface and loads native addons, and the
  [Node compatibility page](../node/) is specific about what is and is not
  there, but Node is Node.
- **A bundler, a test runner and a package manager in one tool.** That is
  Bun's pitch and it is a good one. `sxn` has package commands, but the
  toolchain is not the product here.

## Then what is the language for

Everything above is the runtime, and you can use all of it from ordinary
JavaScript. `.sx` is the other half: JavaScript with mutation and aliasing made
explicit, and TypeScript-style annotations that are not erased but compiled.

```sx
interface Counter { hits: i32; }

function bump(c: &mut Counter): void {
  c.hits += 1;
}

let mut counter: Counter = { hits: 0 };
bump(&mut counter);
console.log(counter.hits);
```

```
1
```

Borrowing an immutable binding is a compile error rather than a convention, and
a declared scalar type changes the code that is generated — `safe let mut n:
i32` wraps at the 32-bit boundary, and a small function with a fully declared
signature is inlined into its caller. [Types and `.sx`](../types/) and
[ownership and borrows](../ownership/) are the two guides for that half.

## Start here

- [Install](../install/) — one command, no dependencies.
- [Quick start](../quickstart/) — a file, a server, and a `.sx` program.
- [Examples](../examples/) — complete programs with their real output.
