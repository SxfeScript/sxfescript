# Install

One command. `sxn` is a single binary with no runtime dependencies to install
alongside it.

## macOS and Linux

```sh
curl -fsSL https://sxfescript.github.io/latest/install.sh | bash
```

## Windows

```powershell
irm https://sxfescript.github.io/latest/install.ps1 | iex
```

Both arm64 and x64 are built for every platform. The script picks the right one.

Release 0.0.2 provides macOS, Linux, and Windows binaries for both arm64 and
x64. See the repository's `llm.txt` for build and verification notes.

## Where it goes

`~/.sxn/bin` on macOS and Linux, `%USERPROFILE%\.sxn\bin` on Windows, and the
installer adds that directory to your `PATH`. Open a new shell, then:

```sh
sxn --version
```

```
sxn 0.0.2
```

## Pinning a version

Swap `latest` for a release tag in either URL to install that version instead
of the newest:

```sh
curl -fsSL https://sxfescript.github.io/v0.0.2/install.sh | bash
```

## Building from source

You need OpenSSL, libcurl, libuv, zlib and libffi on the system. CMake finds
all five and fails clearly, naming the missing one, if any are absent.

Those five are for the whole runtime. Building only the half that embeds —
the WinterTC surface, with no `node:` layer and no libuv — needs OpenSSL,
libcurl and zlib: `cmake --preset minimal`.

```sh
brew install openssl curl libuv zlib libffi          # macOS
```

```sh
apt install libssl-dev libcurl4-openssl-dev libuv1-dev zlib1g-dev libffi-dev
```

Then:

```sh
cmake --preset release
cmake --build --preset release
```

The binary lands at `build/release/sxn`. Use the Release preset for anything
you are going to time. Use Debug for tests: QuickJS gates its leak tracking on
`#ifndef NDEBUG`, so a Release build's leak checks have nothing to detect and
always pass.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

## Next

[Quick start](../quickstart/) — run your first file.
