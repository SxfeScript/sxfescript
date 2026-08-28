# SxfeScript language contract

SxfeScript uses `.sx`. JavaScript is the host language; ordinary JS values keep
ordinary JS semantics. Types that can be erased without generating runtime code
are accepted. JSX, enums, namespaces, decorators, and parameter properties are
rejected.

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

