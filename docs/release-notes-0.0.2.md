# SxfeScript 0.0.2

0.0.2 is the first release with the current SxfeScript/ArcSX runtime surface,
the expanded Node-compatible API, and release binaries for all six supported
platform/architecture combinations.

## Highlights

- Added the documented `Sxn.serve` Request/Response API, including keep-alive,
  request-body bytes, response headers, binding, and `reusePort` support.
- Completed the current minimum Common API and added the remaining registered
  `node:` modules described by the runtime documentation.
- Moved high-frequency Buffer, crypto, URL, filesystem, stream, formatting,
  and inspection paths into native C implementations where profiling showed a
  measurable gain.
- Improved JSON parsing/serialization, including word-at-a-time scanning,
  lower allocation overhead, cycle handling, and numeric formatting.
- Enforced `&mut` ownership checks and compiled declared scalar types instead
  of treating all annotations as comments.
- Added idle cycle sweeping and EventEmitter listener-limit diagnostics.
- Added the browsable guides, runnable examples, Windows PowerShell installer,
  and machine-readable `llm.txt`/`llms.txt` release metadata.

## Performance snapshot

The fresh Linux HTTP run used an AMD Ryzen 7 5700G, 16 threads, Node v23.11.1,
Go 1.26, bombardier, 100,000 requests, and 125 connections per row:

| Test | ExpressX (16 processes) | Winner |
|---|---:|---|
| Static | 165,633 req/s | ExpressX |
| Parameterized | 145,742 req/s | Go net/http (150,195) |
| REST (1 MB JSON POST) | 798 req/s | ExpressX |

ExpressX's single-process rows were 35,404 / 33,066 / 232 req/s for the same
tests. Results vary with machine load; compare rows within one run. The full
report and raw data are maintained in the ExpressX benchmark repository.

## Builds and verification

Release archives are provided for macOS arm64/x64, Linux arm64/x64, and Windows
arm64/x64. macOS and Linux builds pass all 102 CTest cases. Windows binaries
are cross-compiled and verified as PE32+ for their advertised architecture
and as embedding `sxn 0.0.2`; they were not executed on Windows hardware in
this release workspace.
