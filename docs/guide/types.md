# Types and `.sx`

`sxn` runs a plain `.js`, `.mjs`, `.cjs` or `.ts` file directly. A `.sx` module
can `import` any of them and vice versa, and a `.sx` file that uses none of the
extra syntax is just JavaScript with a different extension.

```js
const runtime = typeof Sxn !== "undefined" ? "sxn " + Sxn.version : "something else";
console.log(`hello from ${runtime}`);
```

```sh
sxn hello.js
```

```
hello from sxn 0.0.2
```

## TypeScript, without `tsc`

There is no build step and nothing to configure. `sxn` parses the annotations
itself. Every erasable form is accepted: type aliases, interfaces, `declare`,
type-only exports, annotations, optional parameters, generics on functions,
`as`/`satisfies`, and union types.

Two forms are rejected on purpose rather than stripped. `enum` and `namespace`
both emit a real object at runtime in TypeScript, so quietly removing them
would turn every use of their members into `undefined` — a silent wrong answer
instead of a loud error:

```
SyntaxError: unsupported keyword: enum
```

JSX, decorators, parameter properties, generic classes and non-null assertions
are rejected too, but for a different reason: they are not implemented yet
rather than ruled out.

## The annotations are not thrown away

This is where `.sx` stops being TypeScript-with-a-different-extension. An
annotation emits no code, but the declared type is recorded and spent on the
bytecode that comes out.

**A declared `i32` wraps.** That is the defined semantics for the type, and it
is not JavaScript's:

```sx
function wrap(): number {
  safe let mut n: i32 = 2147483647;
  n += 1;
  return n;
}
console.log(wrap());
```

```
-2147483648
```

Every other annotation, and an un-annotated `safe`, keeps exact JavaScript
arithmetic, where `2147483647 + 1` promotes to a double and gives
`2147483648`. Plain JavaScript is never affected by any of this.

**A declared signature makes a function inlinable.** A small function whose
parameters and return are all declared scalars, and whose body is a single
expression over them, is spliced into its caller rather than called:

```sx
function add2(a: i32, b: i32): i32 { return a + b; }
```

On an M4 that took a two-argument call from 16.3 ns to 5.3, which is where a
hand-written `a + b` lands. Remove the annotations and it is an ordinary call
again. The measurements are in [the performance notes](../performance/).

## `safe`

`safe` is an optional qualifier on `let` and `const`. It marks a binding as
type-stable: the object shape it points at rejects property addition, deletion
and incompatible writes, and the declared scalar type drives the arithmetic
above. Ordinary bindings stay fully dynamic.

```sx
safe let mut total: i32 = 0;
for (let i = 0; i < 10; i++) total += i;
console.log(total);
```

```
45
```

## What to read next

- [Ownership and borrows](../ownership/) — the other half of `.sx`.
- [The language contract](../language/) — the normative rules.
- [Compiling to bytecode](../bytecode/) — skipping the parse entirely.
