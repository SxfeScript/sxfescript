# Ownership and borrows

`.sx` is the same language with mutation and aliasing made explicit. `let mut`
is a mutable owner, `let` an immutable one, `&` borrows a value shared, and
`&mut` borrows it exclusively:

```sx
interface Counter {
  hits: i32;
}

// &mut borrows the counter exclusively, so bump can change what it was
// handed without taking ownership of it.
function bump(c: &mut Counter): void {
  c.hits += 1;
}

let mut counter: Counter = { hits: 0 };
bump(&mut counter);
bump(&mut counter);
console.log(`counter: ${counter.hits}`);
```

```sh
sxn counter.sx
```

```
counter: 2
```

One rule is enforced rather than parsed. `&mut` requires a mutable owner, so
borrowing an immutable one is a compile error:

```sx
let value = 42;
mutate(&mut value);
```

```
SyntaxError: SX2003: cannot borrow immutable binding 'value' as '&mut'; declare it 'let mut'
```

That is a parse error, so it stops the whole file before any of it runs. A
`try`/`catch` in the script will not see it — check the exit status instead.

The rest of the ownership model is parsed but not yet checked. The full
control-flow pass that enforces every rule in
[the language contract](../language/) is still being written, and
[the implementation ledger](../implementation/) tracks exactly what is checked
and what is only parsed. It is worth reading before you rely on a rule being
enforced.

## Fixed-layout structs

An interface whose fields are all primitives — `i32`, `f32`, `f64`, `bool` —
describes a struct with a layout you can rely on: declared field order, natural
alignment, and the same result on every supported target.

```sx
interface Transform {
    x: f32;
    y: f32;
    z: f32;
}

const applyVelocity = (transform: &mut Transform, velocity: &Transform, dt: f32): void => {
    transform.x += velocity.x * dt;
    transform.y += velocity.y * dt;
    transform.z += velocity.z * dt;
};

let mut pos: Transform = { x: 0.0, y: 10.0, z: 5.0 };
let vel: Transform = { x: 1.0, y: 0.0, z: 0.0 };
applyVelocity(&mut pos, &vel, 0.016);
console.log(JSON.stringify(pos));
```

```
{"x":0.016,"y":10,"z":5}
```

That is what code crossing into native memory needs, and the exact rules —
sizes, alignment, padding — are in [the language contract](../language/).

It is also what lets such a value cost nothing. A binding of that type whose
every use `sxn` can account for is compiled to one ordinary local per field, and
no object is allocated, collected, or looked a property up in. JavaScript cannot
be compiled this way — any object might be aliased by something the compiler
cannot see — which is the whole reason for writing the type down.

`pos` above is not one of those, and the reason is the borrow. `&mut pos` hands
`applyVelocity` something it may write through, so the value has to exist as an
object for the length of the call. Written in place instead, the same step is
40 ns against 180:

```sx
let mut pos: Transform = { x: 0.0, y: 10.0, z: 5.0 };
let vel: Transform = { x: 1.0, y: 0.0, z: 0.0 };
pos.x += vel.x * 0.016;
pos.y += vel.y * 0.016;
pos.z += vel.z * 0.016;
```

Both are in [`examples/velocity.sx`](../examples/), side by side.
[Types and `.sx`](../types/) has the rest of the measurement.

## What to read next

- [Types and `.sx`](../types/) — what the annotations do, and what `safe` means.
- [The language contract](../language/) — the normative rules, including the
  ones not yet enforced.
- [Calling C](../native/) — where fixed layout actually pays off.
