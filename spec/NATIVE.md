# Calling native code

Two things in this runtime call into machine code, and they sit on opposite
sides of a line that matters for where ArcSX is going.

| | `Sxn.ffi` | `.node` addons |
|---|---|---|
| Lives in | `src/ffi.c` | `src/napi.c` |
| Installed by | the runtime's own `Sxn` surface (`src/network.c`) | the node: layer (`src/node.c`) |
| Direction | JavaScript calls out to a C function | a C library calls back into the host |
| Backed by | libffi + `dlopen` | Node-API implemented on QuickJS |
| Goes to Rayact | yes | no |

## Which side of the fence, and why

The question that decided this is what happens when ArcSX is folded into
Rayact. Rayact embeds quickjs-ng 0.15.0 — the same base this fork started
from, with about 350 lines of its own on top — so the swap is a small delta
rather than a re-port, and whatever these two features are attached to comes
along with it.

**Rayact already loads native code in its engine core.** `native/core/
rayact_module_abi.h` (ABI version 8) defines a plugin as a shared library
that the loader `dlopen`s to call `rayact_module_register`, and
`native/desktop/plugin_loader.cpp` is compiled verbatim into the Android
`.so` as well as the desktop binary. `native/desktop/` is a misnomer: it is
the shared core, and mobile is a first-class consumer of it. There is no
"desktop-only capability" tier anywhere in that tree. The gating that exists
is per-platform *implementation* — and where a platform cannot `dlopen` at
all, which is iOS, the answer is not "this capability is unavailable" but
`linkage: static` with weak-symbol registration
(`native/ios/ios_plugins_register.cpp`) behind the same entry point.

So calling native code is not, in this family of projects, a desktop luxury.
It is core, and it is gated by *how* a library is linked rather than by
*whether* the capability exists. `Sxn.ffi` belongs there.

**Rayact has no Node layer to put anything in.** Its JavaScript environment
is browser-like: `window`, `self`, a stub `navigator`, and no `process`, no
`Buffer`, no `require`. Its bundler actively steers away from Node builds
(`packages/rayact-dev-server/src/bundler.ts`: *"resolution picks the Node
build of dual-build packages … which crash in QuickJS"*). Putting FFI behind
a Node-compatibility shell would mean building that shell in Rayact for no
other reason, and Node-API in particular would be dead weight next to a
module ABI Rayact already has, with autolinking, manifest platform gating and
SHA-256 artifact verification.

Hence the split. `Sxn.ffi` is an engine capability that travels; the `.node`
loader is Node emulation that does not. A build that wants the runtime
without the Node surface drops `src/napi.c` and the vendored headers and
loses nothing else.

Worth saying plainly, because it is the reason the mobile runtime is treated
as QuickJS rather than as Node: on iOS you cannot `dlopen` code that arrived
after the app was signed. An npm-installed addon is out there no matter how
complete Node-API becomes. A library bundled into the app is not.

## `Sxn.ffi(library, symbol, argTypes[, returnType])`

Returns a callable. The libffi call interface is prepared once, here, so the
returned function is the only thing on the hot path.

```js
const pow = Sxn.ffi("libSystem.B.dylib", "pow", ["f64", "f64"], "f64");
pow(2, 10);                     // 1024
```

An empty library name means this executable, which is how a program reaches
libc and its own symbols without naming a platform-specific file. Handles are
opened once and never closed: a wrapper that outlived its library would call
into unmapped memory, and nothing tracks that lifetime yet.

Types: `void` `bool` `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` `f32` `f64`
`pointer` `cstring`. The C spellings `int`, `unsigned`, `long`, `float`,
`double`, `char`, `size_t`, `ptr` and `string` are accepted too, so a
declaration can be copied out of a header. `void` alone as the argument list
means the function takes nothing.

- 64-bit integers cross as **BigInt** in both directions. A double cannot
  carry one exactly, and silently losing the low bits of a handle or a size
  is worse than making the caller be explicit.
- `pointer` accepts a typed array or an ArrayBuffer and passes the address of
  its bytes — a view passes its own offset — so out-parameters work. `null`
  and `undefined` pass NULL. A BigInt passes as a raw address. A returned
  pointer comes back as a BigInt, or `null`.
- `cstring` converts a JS string to a temporary UTF-8 buffer that lives
  exactly as long as the call. A returned `char*` is copied into a JS string
  and **not freed** — a function that returns owned memory needs its own
  free call, declared separately.

Not supported, and rejected rather than half-done: structs by value,
callbacks into JS, and variadics. Each needs ownership rules this runtime has
not written down (`spec/ABI.md`).

The `unsafe extern` declaration in `spec/LANGUAGE.md` lowers to exactly this
call:

```sx
unsafe extern pow(f64, f64): f64 from "libSystem.B.dylib";
// const pow = Sxn.ffi("libSystem.B.dylib", "pow", "f64, f64", "f64");
```

## `.node` addons

`require("./thing.node")` works, and so does `process.dlopen(module, path)`,
which is what `require` calls.

The shape of this problem is the reverse of FFI, and it is worth stating
because "we have FFI, so we can load addons" does not follow. An addon
exports one symbol, `napi_register_module_v1`, and *imports* around seventy
`napi_*` functions that the host must provide — `next-swc` imports 67, sharp
53. Loading one is not a matter of calling into a library; it is a matter of
being the library it calls into. So `src/napi.c` is ordinary exported C, and
the executable is linked with `ENABLE_EXPORTS` so `dlopen` can resolve those
imports back to it.

`third_party/node-api/` holds Node's own four headers, copied verbatim, so an
addon sees exactly the declarations it was compiled against.

A `napi_value` is a JSValue owned by the innermost handle scope. QuickJS is
refcounted rather than tracing, so a scope is a plain array of values it
releases on close — simpler than the same thing on V8, and it means an addon
that leaks handles leaks memory rather than corrupting anything.

Implemented: values and coercions, properties and elements, functions,
callbacks with their own scope, constructors, errors and the pending-exception
protocol, handle and escapable scopes, references, `napi_wrap`/`unwrap`,
externals, ArrayBuffers, typed arrays, Buffers, promises, and async work on
libuv's thread pool.

Not implemented:

- **Thread-safe functions.** An addon's own threads calling into JS needs a
  cross-thread queue draining on the loop thread. They return a failure with
  that as the message rather than accepting the call and dropping it.
- **Weak references.** `napi_create_reference` with a count of zero is kept
  strong, because QuickJS has no weak handle that can be resurrected. That
  leaks rather than dangles.
- **The old V8 `NODE_MODULE` interface.** An addon built against it is
  refused by name; there is no path to it that does not embed V8.
