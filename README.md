# SxfeScript and SXN

SXN is a standalone QuickJS-based runtime for `.sx` systems code and ordinary
JavaScript. SxfeScript adds explicit mutation, affine values, borrows, and
erasable TypeScript-style annotations without a Vite or AOT build step.

This repository is intentionally independent from Rayact. Its QuickJS source
was a direct snapshot of Rayact's customized fork at commit `66f4965`, and has
since diverged under its own name, **ArcSX** (see
`third_party/QUICKJS-PROVENANCE.md` for the full lineage).

A pitch-and-explainer site for both -- SxfeScript against TypeScript, what
ArcSX actually runs, and what's open for debate versus fixed -- lives at
[sxfescript.github.io](https://sxfescript.github.io), built from
[`docs/`](docs/index.html) and published from a separate repo,
[SxfeScript/sxfescript.github.io](https://github.com/SxfeScript/sxfescript.github.io)
(`scripts/publish-docs.sh`). **Contributions, including disagreement with the
current design, are welcome** -- see [`CONTRIBUTING.md`](CONTRIBUTING.md) for
what's genuinely open and the one constraint that isn't (no JIT, for mobile).

## Documentation

[**sxfescript.github.io/docs**](https://sxfescript.github.io/docs/) is every
one of these markdown files rendered as a browsable site, generated from this
repo by `scripts/publish-docs.sh` so a spec edit is a docs edit. Start with
the [quick start](https://sxfescript.github.io/docs/quickstart/), or the
[examples](https://sxfescript.github.io/docs/examples/) if you'd rather read
code first. There is an [`llms.txt`](https://sxfescript.github.io/llms.txt)
index and a single-file
[`llms-full.txt`](https://sxfescript.github.io/llms-full.txt) for tooling.

Everything past what's here -- the language, the ABI, the runtime and Node
surfaces, native calling, bytecode, and the full performance write-up behind
the two tables below -- lives in [`spec/`](spec/); see that directory's own
files for each topic.

Complete programs that run as-is are in [`examples/`](examples/):

| File | What it shows |
|---|---|
| [`hello.sx`](examples/hello.sx) | Erasable types, `let mut`, and an `&mut` borrow |
| [`velocity.sx`](examples/velocity.sx) | A primitive-only interface as a fixed-layout struct |
| [`server.mjs`](examples/server.mjs) | `Sxn.serve` with `Request`/`Response` routing and a JSON body |
| [`fetch.mjs`](examples/fetch.mjs) | `fetch`, then the same response read as a stream |
| [`files.mjs`](examples/files.mjs) | `Sxn.file`/`Sxn.write`, and `node:fs` over the same file |
| [`ffi.mjs`](examples/ffi.mjs) | Calling a C function through `Sxn.ffi` |

## Install

macOS/Linux (arm64 or x64):

```sh
curl -fsSL https://sxfescript.github.io/latest/install.sh | bash
```

Windows (arm64 or x64):

```powershell
irm https://sxfescript.github.io/latest/install.ps1 | iex
```

Both install to `~/.sxn/bin` (`%USERPROFILE%\.sxn\bin` on Windows) and add it
to your PATH. Swap `latest` for a version tag (`v0.0.1`) in either URL to pin
a specific release instead of always getting the newest one.

## Build

Needs OpenSSL, libcurl, libuv, zlib, and libffi on the system (`brew install
openssl curl libuv zlib libffi` on macOS; `apt install libssl-dev libcurl4-openssl-dev
libuv1-dev zlib1g-dev libffi-dev` on Debian/Ubuntu). CMake finds all five and
fails clearly, naming the missing one, if any aren't there.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the example:

```sh
./build/debug/sxn examples/velocity.sx
```

**Run the tests against a Debug build.** QuickJS gates its leak tracking on
`#ifndef NDEBUG` (`ENABLE_DUMPS` in `third_party/quickjs/quickjs.c`), so in a
Release build the `sxn-leak-check` test still runs but has nothing to detect
and always passes. A Debug build is what actually catches a leaked atom,
object or string -- an atom leak in the `node:*` layer sat unnoticed behind a
green Release run until it aborted the first Debug one.

## Current implementation status

The repository contains a working QuickJS-backed CLI, an in-memory `.sx`
frontend, fixed-layout arena primitives, package workflow commands, an LSP
transport, VS Code language packaging, specifications, and tests. The native
opcode lowering, full control-flow ownership pass, native npm registry backend,
and semantic LSP features are tracked in `spec/IMPLEMENTATION.md` and are not
yet represented as complete production implementations.

## What the runtime does

Two documents cover what actually runs, and split the same way the codebase
does:

- **`spec/RUNTIME.md`** -- the WinterCG web APIs and the `Sxn` host namespace:
  `fetch`, `Sxn.serve` (HTTP, SSE, WebSocket upgrade), Web Streams, Web
  Crypto, `structuredClone`, and `Sxn.ffi` for calling a C function directly.
  This is the half that travels when the engine is embedded elsewhere, and
  the only half a mobile build needs.
- **`spec/NODE.md`** -- what makes `sxn` usable as a Node alternative:
  CommonJS, `node:` builtins (24 of ~37), and `.node` native-addon loading
  through a from-scratch Node-API implementation. This half exists to
  emulate Node and nothing else, so a build with no Node surface drops it
  and loses nothing on the runtime side.

`spec/NATIVE.md` is the design note behind that split, written against a
concrete question: when this engine is folded into Rayact, which of `Sxn.ffi`
and `.node`-addon loading goes with it. (Answer: `Sxn.ffi`, because Rayact
already loads native code in its engine core on every platform including
mobile, and has no Node layer to put an addon loader in.)

A third document, **`spec/BYTECODE.md`**, covers `.sxbc`: `sxn compile
app.js` produces bytecode for distribution (`--strip` drops the compiling
machine's own paths from it), `sxn --compile-cache app.js` compiles once and
reuses the result on later launches, and `sxn app.sxbc` runs either one
directly. Real, measured gains -- see that document for the numbers -- and
proportional to how much there is to parse: noticeable on a large file,
negligible on a one-liner.

## Benchmarks: sxn vs Node vs Bun

`benchmarks/wintercg/run.sh` runs matched WinterCG-style workloads against
`sxn`, Node and Bun side by side. No category is
hidden -- the others win the ones you'd expect them to. Each runtime runs the
same workload with the same iteration counts, written in that runtime's
idiomatic form (`Bun.serve`/`Bun.env` for Bun, `Sxn.serve` for sxn); Buffer,
TextEncoder and EventEmitter are the APIs under test and are the same in all
three. Bun is optional -- its rows are skipped with a note if it isn't
installed.

```sh
sh benchmarks/wintercg/run.sh
```

For performance measurements, use the optimized binary explicitly; the script
accepts any SXN path. For example:

```sh
RUNS=1000 SXN=build/release/sxn sh benchmarks/wintercg/run.sh
```

Keep Debug for leak and correctness checks; Release is the appropriate binary
for throughput, startup, and pause timing.

### The two machines

Everything below was measured on both, because a single machine can flatter a
runtime and neither of these is neutral: the Mac is the faster chip but a
working laptop under load, and the Linux box is slower per core but idle.

| | **Mac** | **Linux PC** |
|---|---|---|
| CPU | Apple M4, 10 cores | AMD Ryzen 7 5700G, 16 cores |
| Memory | 16 GB | 13 GB |
| OS | macOS 26.6.2 (arm64) | Ubuntu 23.10, kernel 6.5.0-44 (x86_64) |
| Compiler | Apple clang | gcc 13.2 |
| Node | v25.2.1 | v18.13.0 |
| Bun | 1.2.17 | 1.2.17 |
| Load while measuring | 2-5 | 0.4-1.2 |

Read each machine's table against itself, never across the two. The Linux
Node is four major versions behind, and `performance.now` costs far more per
call on that kernel, which is why its pause totals read in seconds for all
three runtimes. Same tree, same tests, same 66 fixtures passing on both.

How each row is measured: throughput rows are the harness's own 1,000-run
medians. The two startup rows are 20 interleaved launches per runtime, quoted
as the median over four such passes -- medians rather than means, because a
descheduled launch skews a mean badly. Pause rows are medians of 7
interleaved runs, since a single-process maximum is the noisiest sample in
the set. Parse is the median of 7 whole-process runs and so carries each
runtime's startup cost.

### Mac (Apple M4)

| Category | sxn | Node | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task | **10.4 ms** | 76.3 ms | 15.5 ms | sxn |
| Cold start | **8.4 ms** | 41.6 ms | 9.2 ms | sxn |
| Sustained throughput: Buffer ops | **19.2 ms** | 23.8 ms | 27.6 ms | sxn |
| Sustained throughput: TextEncoder | **4.7 ms** | 38.9 ms | 6.3 ms | sxn |
| Sustained throughput: EventEmitter | 6.6 ms | **5.1 ms** | 9.3 ms | Node |
| Pause consistency: total time | **147.8 ms** | 242.5 ms | 283.1 ms | sxn |
| Pause consistency: worst single pause | **0.04 ms** | 0.36 ms | 2.59 ms | sxn |
| Parse 32k-line generated file | **20.9 ms** | 51.0 ms | 24.3 ms | sxn |

Seven of eight, holding steady since the last pass -- these numbers include
the class-constructor and thread-safe-function work, and neither moved a
row. EventEmitter is the one Node keeps, and its 1.1x here is a JIT inlining
a call to nothing: an ablation that skips the fused call's guards entirely
still only reaches 4.7 ms, because roughly a third of the row is this
interpreter's own loop dispatch.

### Linux PC (Ryzen 7 5700G)

| Category | sxn | Node 18 | Bun | Winner |
|---|---|---|---|---|
| Real-world end-to-end task | **6.9 ms** | 224.0 ms | 23.2 ms | sxn |
| Cold start | **7.6 ms** | 117.1 ms | 15.1 ms | sxn |
| Sustained throughput: Buffer ops | **37.4 ms** | 75.6 ms | 83.0 ms | sxn |
| Sustained throughput: TextEncoder | **8.6 ms** | 89.2 ms | 16.2 ms | sxn |
| Sustained throughput: EventEmitter | 14.8 ms | **13.0 ms** | 23.2 ms | Node |
| Pause consistency: total time | **2836.0 ms** | 3463.2 ms | 3219.4 ms | sxn |
| Pause consistency: worst single pause | **0.30 ms** | 4.96 ms | 5.67 ms | sxn |
| Parse 32k-line generated file | **34.8 ms** | 144.3 ms | 54.1 ms | sxn |

Seven of eight, and the numbers are far steadier than anything the laptop can
produce. Both machines agree on which row is which: sxn takes everything
except EventEmitter, and that one is Node's on both, which is the point --
it is the one row where the gap is architectural rather than incidental. The
Linux gap is the narrower of the two, 1.1x against the Mac's 1.3x.

The full write-up -- pause-row detail, the no-JIT tradeoff, every
optimization behind these numbers in the order it landed, and what's still
open -- is in [`spec/PERFORMANCE.md`](spec/PERFORMANCE.md).
