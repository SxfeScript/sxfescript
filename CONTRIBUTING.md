# Contributing

This project is young, opinionated in places, and looking for people willing
to push back on those opinions. If something in the specs or the code reads
as "obviously the right call" and you don't think it is, that's exactly the
kind of issue worth opening.

The full pitch — what SxfeScript adds over TypeScript, what ArcSX is, and
what's open for debate versus fixed — is on the project's documentation
site, built from [`docs/index.html`](docs/index.html) and published by
[`.github/workflows/docs.yml`](.github/workflows/docs.yml). The short version:

**Genuinely open to change**, including disagreement with the current
approach: the shape and scope of `safe`/`unsafe`, which TypeScript forms get
real support next, ownership and borrow-checking rules and their error
messages, `node:`/WinterTC coverage priorities, naming, ergonomics — and the
underlying ideas themselves. If you think the ownership model solves the
wrong problem, open an issue and make the case.

**Not open to change:** ArcSX has no JIT and never will. iOS does not grant
JIT entitlements to third-party apps, so a machine-code tier is not a slower
version of this runtime on that platform — it's an absent one. That
constraint is why this project exists in this shape; it isn't a preference
up for a vote.

## Where the design lives

The `spec/` directory is the actual design surface, not settled history:

- [`spec/LANGUAGE.md`](spec/LANGUAGE.md) — the SxfeScript language contract
- [`spec/ABI.md`](spec/ABI.md) — the native/JS boundary
- [`spec/NATIVE.md`](spec/NATIVE.md) — `Sxn.ffi` vs `.node` addons, and why they're split
- [`spec/RUNTIME.md`](spec/RUNTIME.md) / [`spec/NODE.md`](spec/NODE.md) — what's supported, as a runtime and as a Node alternative
- [`spec/IMPLEMENTATION.md`](spec/IMPLEMENTATION.md) — what's real today, what's measured, what's still open
- [`spec/BYTECODE.md`](spec/BYTECODE.md) — precompiled `.sxbc` bytecode
- [`spec/PERFORMANCE.md`](spec/PERFORMANCE.md) — the benchmark numbers in the README, explained

## Working in the repo

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run tests against a Debug build — QuickJS's leak tracking is compiled out of
Release, so `ctest --preset release` can't catch a leaked atom or object the
way Debug does. See the README's Build section for system dependencies.

A pull request that changes behavior should come with a fixture under
`tests/fixtures/` and a `ctest` registration in `CMakeLists.txt` — most
existing fixtures assert against Node's own output for the same code, which
is worth matching where the change touches Node compatibility.
