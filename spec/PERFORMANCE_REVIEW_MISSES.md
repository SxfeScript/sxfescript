# Performance review misses

This document records three issues missed during the tierless-fusion work and
the Linux cross-check. It is a fixing checklist, not a rejection of the
measured performance gains: the Linux Release suite passed all 38 tests and
the throughput improvements reproduced independently.

## 1. `OP_call` enters method-name tracking

**Location:** `third_party/quickjs/quickjs.c`, in `resolve_labels`, currently
around lines 38251-38255.

### Observed

`case OP_call` falls through to the new `OP_get_field2` method-name tracking
code:

```c
case OP_call:
case OP_get_field2:
    if (mname_depth < (int)countof(mname_stack))
        mname_stack[mname_depth++] = get_u32(bc_buf + pos + 1);
```

`OP_call` has a 16-bit argument-count operand, not a 32-bit atom operand. The
read therefore consumes bytes belonging to following bytecode and pushes an
invalid value onto `mname_stack`. Separating `OP_call` from the former shared
`OP_call`/`OP_call_method` handling also bypasses the existing plain-call
tail-call transformation.

### Impact

- A compiler-side out-of-bounds or cross-instruction read is possible near the
  end of a bytecode buffer.
- Nested ordinary calls can misalign method-name tracking and suppress valid
  length or emit fusions.
- A stale method name can be associated with a later method call. Runtime
  builtin-identity guards make a semantic miscompile unlikely, but the
  recognizer should not depend on those guards to correct corrupt metadata.
- Plain calls in return position no longer receive their previous tail-call
  transformation.

### Required correction

- Restore `OP_call` to its original call/tail-call optimization path.
- Restrict atom pushes to `OP_get_field2`; `OP_get_array_el2` should continue
  pushing the dynamic-name sentinel.
- Ensure every tracked method-producing operation has exactly one matching
  `OP_call_method` pop, including nested argument expressions.

### Regression tests

- Dump bytecode for direct calls in return position and assert that the
  tail-call form is retained.
- Cover `Buffer.from(helper(), "utf-8").length` and
  `encoder.encode(helper()).length` followed by unrelated method calls.
- Cover nested static and computed method calls in argument expressions.
- Run ASan/UBSan while compiling minimal functions ending in a direct call.

## 2. `Buffer.byteLength` differs from Node

**Location:** `src/node.c`, `js_buffer_byte_length`, currently around lines
934-972.

### Observed

The non-string path reads a public `byteLength` property from any value and
returns it when numeric. Node accepts only a Buffer, ArrayBuffer,
SharedArrayBuffer, TypedArray, or DataView. An arbitrary object such as
`{ byteLength: 4 }` must throw `TypeError`; a getter must not be invoked as a
substitute for the required brand check.

Encoding dispatch also compares only lowercase atoms. Node treats supported
encoding names case-insensitively, so these results must match their lowercase
forms:

```js
Buffer.byteLength("aGVsbG8=", "BASE64") // 5
Buffer.byteLength("abc", "HEX")         // 1
```

The current implementation treats both names as unknown and falls back to
UTF-8 length.

### Required correction

- Use actual QuickJS class and typed-array/view checks rather than reading an
  arbitrary `byteLength` property.
- Support Buffer, ArrayBuffer, SharedArrayBuffer when available, TypedArray,
  and DataView with their real byte lengths.
- Reject all other non-string values with `TypeError` without invoking a
  user-defined `byteLength` getter.
- Normalize encoding names using the same case-insensitive rules as the rest
  of the Buffer compatibility layer before dispatch.
- Preserve Node's unknown-encoding fallback to UTF-8.

### Regression tests

- Differentially compare sxn and Node for every accepted binary-data class.
- Add `{ byteLength: 4 }`, a throwing `byteLength` getter, arrays, numbers,
  `null`, and proxies around invalid objects.
- Test lowercase, uppercase, and mixed-case forms of UTF-8, hex, base64,
  base64url, latin1, binary, ASCII, UCS-2, and UTF-16LE.
- Retain the unpaired-surrogate and odd-length hex cases already covered.

## 3. The Linux benchmark command is not self-reproducing

**Location:** `benchmarks/wintercg/run.sh`, currently around lines 12-25.

### Observed

Running the documented command through noninteractive SSH on the Linux host:

```sh
ssh owner@10.0.0.43 \
  'cd /home/owner/sxfescript && RUNS=101 SXN=build/release/sxn sh benchmarks/wintercg/run.sh'
```

failed at the first startup measurement with `time: not found`. The script is
advertised for `/bin/sh` but relies on a `time` implementation that is not
available in that environment.

Bun is installed at `/home/owner/.bun/bin/bun`, but that directory is absent
from the noninteractive SSH `PATH`. The harness consequently reports Bun as
unavailable even though the documented Linux table contains Bun results.

### Required correction

- Use `benchmarks/wintercg/startup20.py` (or another checked-in portable timer)
  for startup and real-world wall-clock measurements instead of shell `time`.
- Add a `BUN` override parallel to `SXN`, defaulting to `bun` when it is found
  on `PATH`.
- Perform availability checks and invocations through the resolved `BUN`
  command.
- Keep Bun optional, but print the exact resolved runtime paths and versions
  at the start of a measurement run.

### Regression tests

- Run the documented command through noninteractive SSH with a minimal PATH.
- Run with `BUN=/home/owner/.bun/bin/bun` and confirm all three runtimes are
  measured.
- Run on macOS and Linux under `/bin/sh` without relying on shell-specific
  timing syntax.
- Confirm server cleanup still happens if a timed command fails.

## Verified performance context

The Linux Release build passed 38/38 CTest tests. A separate 31-run median
check against the current Linux binary produced:

| Runtime | Buffer | TextEncoder | EventEmitter |
|---|---:|---:|---:|
| sxn | 42.1 ms | 10.3 ms | 16.5 ms |
| Node | 75.5 ms | 89.0 ms | 12.9 ms |
| Bun | 81.0 ms | 16.4 ms | 23.9 ms |

These measurements support the reported Linux performance direction: sxn won
Buffer and TextEncoder and beat Bun on EventEmitter, while Node retained the
EventEmitter lead. They do not remove the compiler correctness, API
compatibility, or harness reproducibility issues above.

