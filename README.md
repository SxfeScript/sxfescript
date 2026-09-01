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

Complete programs that run as-is are in [`examples/`](examples/), all of them
`.sx`:

| File | What it shows |
|---|---|
| [`hello.sx`](examples/hello.sx) | Erasable types, `let mut`, and an `&mut` borrow |
| [`velocity.sx`](examples/velocity.sx) | A primitive-only interface as a fixed-layout struct |
| [`server.sx`](examples/server.sx) | `Sxn.serve` with `Request`/`Response` routing and a JSON body |
| [`fetch.sx`](examples/fetch.sx) | `fetch`, then the same response read as a stream |
| [`files.sx`](examples/files.sx) | `Sxn.file`/`Sxn.write`, and `node:fs` over the same file |
| [`ffi.sx`](examples/ffi.sx) | Calling a C function through `Sxn.ffi` |

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

- **`spec/RUNTIME.md`** -- the WinterTC web APIs and the `Sxn` host namespace:
  `fetch`, `Sxn.serve` (HTTP, SSE, WebSocket upgrade), Web Streams,
  `URLPattern`, Web Crypto, `structuredClone`, and `Sxn.ffi` for calling a C
  function directly. Every name in the Minimum Common API is there except
  WebAssembly's.
  This is the half that travels when the engine is embedded elsewhere, and
  the only half a mobile build needs.
- **`spec/NODE.md`** -- what makes `sxn` usable as a Node alternative:
  CommonJS, `node:` builtins (37 of ~37), and `.node` native-addon loading
  through a from-scratch Node-API implementation. This half exists to
  emulate Node and nothing else, so a build with no Node surface drops it
  and loses nothing on the runtime side.

`spec/NATIVE.md` is the design note behind that split, written against a
concrete question: when this engine is folded into Rayact, which of `Sxn.ffi`
and `.node`-addon loading goes with it. (Answer: `Sxn.ffi`, because Rayact
already loads native code in its engine core on every platform including
mobile, and has no Node layer to put an addon loader in.)

A third document, **`spec/BYTECODE.md`**, covers `.sxbc`: `sxn compile
app.sx` produces bytecode for distribution (`--strip` drops the compiling
machine's own paths from it), `sxn --compile-cache app.sx` compiles once and
reuses the result on later launches, and `sxn app.sxbc` runs either one
directly. Real, measured gains -- see that document for the numbers -- and
proportional to how much there is to parse: noticeable on a large file,
negligible on a one-liner.

## Benchmarks: sxn vs Node vs Bun

`benchmarks/wintertc/run.sh` runs matched WinterTC-style workloads against
`sxn`, Node and Bun side by side. No category is
hidden -- the others win the ones you'd expect them to. Each runtime runs the
same workload with the same iteration counts, written in that runtime's
idiomatic form (`Bun.serve`/`Bun.env` for Bun, `Sxn.serve` for sxn); Buffer,
TextEncoder and EventEmitter are the APIs under test and are the same in all
three. Bun is optional -- its rows are skipped with a note if it isn't
installed.

```sh
sh benchmarks/wintertc/run.sh
```

For performance measurements, use the optimized binary explicitly; the script
accepts any SXN path. For example:

```sh
RUNS=1000 SXN=build/release/sxn sh benchmarks/wintertc/run.sh
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
| Node | v25.2.1 | v23.11.1 |
| Bun | 1.2.17 | 1.2.21 |
| Load while measuring | 2-5 | 0.4-1.2 |

Read each machine's table against itself, never across the two. Both now run
the same major Node; the Linux box's Bun is a few patches ahead. What still
differs is the kernel: `performance.now` costs far more per call there, which
is why its pause totals read in seconds for all three runtimes. Same tree,
same tests, same 95 fixtures passing on both.

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
| Real-world end-to-end task | **8.4 ms** | 76.6 ms | 15.6 ms | sxn |
| Cold start | **7.5 ms** | 42.5 ms | 9.4 ms | sxn |
| Sustained throughput: Buffer ops | **19.4 ms** | 24.5 ms | 27.1 ms | sxn |
| Sustained throughput: TextEncoder | **4.7 ms** | 39.8 ms | 6.2 ms | sxn |
| Sustained throughput: EventEmitter | 6.7 ms | **5.4 ms** | 9.2 ms | Node |
| Sustained throughput: JSON round trip | 48.0 ms | 29.3 ms | **24.8 ms** | Bun |
| Pause consistency: total time | **146.6 ms** | 241.7 ms | 277.0 ms | sxn |
| Pause consistency: worst single pause | **0.01 ms** | 0.28 ms | 3.13 ms | sxn |
| Parse 32k-line generated file | **20.1 ms** | 49.9 ms | 25.6 ms | sxn |

Seven of nine. The two that are not sxn's are the two worth reading: a JIT
inlines an EventEmitter call to nothing, and an ablation that skips this
interpreter's fused-call guards entirely still only reaches 4.7 ms, because
roughly a third of that row is loop dispatch. JSON is a megabyte parsed and
written back forty times, and the gap there is the same story with more code
in it -- `JSON.parse` is C in all three, but what surrounds it is not.

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
