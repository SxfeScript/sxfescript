# SxfeScript language contract

SxfeScript uses `.sx`. JavaScript is the host language; ordinary JS values keep
ordinary JS semantics. Types that can be erased without generating runtime code
are accepted: type aliases, interfaces, `declare`, type-only exports,
annotations, optional parameters, generics on functions, `as`/`satisfies`, and
union types. `enum` and `namespace` are rejected deliberately rather than
erased -- both emit a runtime object in real TypeScript, so silently stripping
them would turn every use of their members into `undefined`. JSX, decorators,
parameter properties, generic classes, and non-null assertions are rejected
because they are simply not implemented yet, not because they've been ruled
out.

`safe` is an optional contextual qualifier for `let` and `const`. It marks a
binding as type-stable for runtime validation and optimization; ordinary
bindings remain dynamic. `safe let` follows the ownership rules below, while
`safe let mut` allows reassignment only within its declared or inferred type.
Safe object shapes reject property addition/deletion and incompatible writes.
The compatibility transformer erases this qualifier; the native parser is
responsible for attaching its runtime descriptor.

Primitive FFI declarations use an explicit unsafe boundary:

```sx
unsafe extern add(i32, i32): i32 from "add.dylib";
```

Native parsing does not implement this lowering yet and rejects `extern`
declarations with an explicit "not yet supported" error rather than
mis-parsing them; the standalone compatibility transformer (src/frontend.c)
lowers the declaration to

```js
const add = Sxn.ffi("add.dylib", "add", "i32, i32", "i32");
```

which is a call that now works -- see `spec/NATIVE.md` for the type list and
what it does with pointers and strings. Structs by value, callbacks and
variadics are still rejected there, because each needs ownership rules this
document has not written down.

## Ownership

- `let mut value: T` creates a mutable owner.
- `let value: T` creates an immutable owner.
- `&value` creates a shared lexical borrow.
- `&mut value` creates an exclusive lexical borrow and requires a mutable owner.
- Passing, assigning, returning, or capturing an affine value by value moves it.
- A borrow cannot be returned, stored in a longer-lived value, or captured.
- `unsafe` permits typed JS/native interop but never disables runtime alias locks.

`i32`, `f32`, `f64`, `bool`, and ordinary JavaScript values are copyable.
Primitive-only interfaces define affine fixed-layout structs. A literal becomes
such a struct only in an explicit annotation, typed argument, or typed return
context.

At control-flow joins, a value moved on any reachable branch is considered
moved. Loop-carried owners must be reinitialized on every continuation path.
Borrowed affine values cannot cross `await`.

## Layout

Fields retain declaration order. `bool` has size/alignment 1, `i32` and `f32`
have size/alignment 4, and `f64` has size/alignment 8. Each field and final
struct size are padded to natural alignment. This layout is identical on all
supported desktop targets.
