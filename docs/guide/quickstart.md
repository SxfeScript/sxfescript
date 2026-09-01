# Quick start

`sxn` is a single binary. It runs `.sx` — this project's own language — plus
`.ts`, `.js`, `.mjs` and `.cjs`, all directly, with no build step and nothing
to configure first.

If you have not installed it yet, that is [one command](../install/) and takes
a few seconds. Check it landed:

```sh
sxn --version
```

```
sxn 0.0.1
```

## Your first program

Put this in `hello.sx`:

```sx
interface Repo {
  name: string;
  stars: i32;
}

const describe = (repo: Repo): string =>
  `${repo.name} has ${repo.stars} star${repo.stars === 1 ? "" : "s"}`;

console.log(describe({ name: "sxfescript", stars: 1 }));
```

```sh
sxn hello.sx
```

```
sxfescript has 1 star
```

That is an ordinary interface and an ordinary annotation, and there is no
`tsc` and no bundler in front of it. `sxn` parses the types itself and emits
no code for them — but it does not throw them away either: a declared scalar
type is remembered and spent on the bytecode that comes out. See
[types and `.sx`](../types/) for what that buys.

## Ownership, in one example

`.sx` adds explicit mutation and aliasing. `let mut` is a mutable owner, `let`
an immutable one, `&` borrows shared and `&mut` borrows exclusively:

```sx
interface Counter { hits: i32; }

function bump(c: &mut Counter): void {
  c.hits += 1;
}

let mut counter: Counter = { hits: 0 };
bump(&mut counter);
bump(&mut counter);
console.log(`counter: ${counter.hits}`);
```

```
counter: 2
```

Borrowing an immutable binding is a compile error, not a convention:

```
SyntaxError: SX2003: cannot borrow immutable binding 'value' as '&mut'; declare it 'let mut'
```

[Ownership and borrows](../ownership/) is the full guide.

## An HTTP server

```sx
const server = Sxn.serve({ port: 3000 }, (req: Request): Response => {
  return new Response("hello from " + new URL(req.url).pathname);
});

console.log(`listening on ${server.url}`);
```

```sh
sxn server.sx
```

```
listening on http://127.0.0.1:3000
```

[An HTTP server](../http-server/) covers request bodies, routing, keep-alive
and `node:http`.

## Where to go next

- [Types and `.sx`](../types/) — TypeScript with no build step, and what the
  annotations do that TypeScript's do not.
- [Using node: packages](../node-packages/) — running an existing Node project.
- [Examples](../examples/) — complete programs with their real output.
- [The CLI](../cli/) — every command and flag.
