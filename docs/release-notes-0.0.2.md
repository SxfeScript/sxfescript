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

| Framework | Static | Parameterized | REST (1 MB JSON POST) |
|---|---:|---:|---:|
| **ExpressX (16 processes)** | **168,773** | **155,380** | **848** |
| Go net/http | 156,218 | 149,622 | 723 |
| Fastify (16 processes) | 90,623 | 86,201 | 536 |
| Koa (16 processes) | 68,929 | 65,094 | 725 |
| Express (16 processes) | 36,032 | 35,314 | 765 |
| ExpressX (1 process) | 38,028 | 34,201 | 232 |
| Express (1 process) | 8,003 | 7,841 | 220 |

ExpressX takes all three, by 8% on Static and 4% on Parameterized against
Go, and 11% on REST against Express. Go uses every core from one process, so
the 16-process rows are the fair comparison against it. Results vary with
machine load; compare rows within one run. The full report and raw data are
maintained in the ExpressX benchmark repository.

## Builds and verification

Release archives are provided for macOS arm64/x64, Linux arm64/x64, and Windows
arm64/x64. macOS and Linux builds pass all 102 CTest cases. Windows binaries
are cross-compiled and verified as PE32+ for their advertised architecture
and as embedding `sxn 0.0.2`; they were not executed on Windows hardware in
this release workspace.
