# Benchmarks

`sxn` against Node and Bun on the same workloads, on two machines.

sxn takes seven of the eight categories on both machines. The eighth is Node's
on both, and that row is the honest one: it is architectural rather than
incidental, and [the performance notes](../performance/) say why.

Every number here comes out of `benchmarks/wintertc/run.sh`, which is in the
repo and which you can run yourself:

```sh
sh benchmarks/wintertc/run.sh
```

It runs matched WinterTC-style workloads against all three runtimes. Each one
runs the same workload with the same iteration counts, written in that
runtime's idiomatic form — `Bun.serve`/`Bun.env` for Bun, `Sxn.serve` for sxn.
Buffer, TextEncoder and EventEmitter are the APIs under test and are the same
in all three. Bun is optional; its rows are skipped with a note if it is not
installed. For a measurement run, point the script at the optimized binary:

```sh
RUNS=1000 SXN=build/release/sxn sh benchmarks/wintertc/run.sh
```

The tables below are included from the repo's own `README.md` when this page is
built, so the site and the repo cannot disagree about a measured number.

### Mac (Apple M4)

<!-- include-section: README.md#mac-apple-m4 table -->

### Linux PC (Ryzen 7 5700G)

<!-- include-section: README.md#linux-pc-ryzen-7-5700g table -->

Both machines agree on which row is which: sxn takes everything except
EventEmitter, and that one is Node's on both, which is the point — it is the
one row where the gap is architectural rather than incidental.

## The machines, and how to read the tables

<!-- include-section: README.md#the-two-machines body -->

## Going deeper

- [Performance notes](../performance/) — the full write-up: what each row
  measures, every optimization behind these numbers in the order it landed,
  the ceilings that measured zero, and what is still open.
- [Benchmark references](../benchmark-references/) — where every third-party
  figure quoted in these docs comes from.
- [Implementation ledger](../implementation/) — what is real today and what is
  not yet.
