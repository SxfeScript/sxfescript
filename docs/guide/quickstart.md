# Quick start

`sxn` is a single binary. It runs `.sx` — this project's own language — plus
`.ts`, `.js`, `.mjs` and `.cjs`, all directly, with no build step and nothing
to configure first.

## Install

macOS and Linux, arm64 or x64:

```sh
curl -fsSL https://sxfescript.github.io/latest/install.sh | bash
```

Windows, arm64 or x64:

```powershell
irm https://sxfescript.github.io/latest/install.ps1 | iex
```

Both drop the binary in `~/.sxn/bin` (`%USERPROFILE%\.sxn\bin` on Windows) and
add that to your `PATH`. Open a new shell, then check it:

```sh
sxn --version
```

```
sxn 0.0.1
```

Every release also ships plain `.tar.gz` and `.zip` archives on the
[releases page](https://github.com/SxfeScript/sxfescript/releases), if you'd
rather unpack one yourself.

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
`tsc` and no bundler in front of it. `sxn` parses the types itself and strips
them as it goes.

## Ownership and borrows

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

An interface whose fields are all primitives — `i32`, `f32`, `f64`, `bool` —
describes a fixed-layout struct: declared field order, natural alignment, the
same layout on every supported target. That is what code crossing into native
memory needs.

The syntax is parsed natively today. The full control-flow ownership pass that
enforces every rule in [the language contract](../language/) is still being
written, and [the implementation ledger](../implementation/) tracks exactly
what is checked and what is only parsed. It is worth reading before you rely
on a rule being enforced.

## An HTTP server

`Sxn.serve` hands your function a `Request` and expects a `Response` back —
the same pair of objects a handler gets on Cloudflare Workers, Deno or Bun:

```sx
const server = Sxn.serve({ port: 3000 }, async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  if (url.pathname === "/echo") return Response.json(await req.json());
  return new Response("hello from " + url.pathname);
});

console.log(`listening on ${server.url}`);
```

```sh
sxn server.sx
```

```
listening on http://127.0.0.1:3000
```

`port: 0` asks the operating system for a free port instead, and
`server.port` then tells you which one it picked. `server.stop()` shuts the
listener down, so one process can serve and then go on to do something else.

## JavaScript and TypeScript run too

Nothing above is required. `sxn` runs a plain `.js`, `.mjs`, `.cjs` or `.ts`
file directly, and a `.sx` module can `import` any of them and vice versa. A
`.sx` file that uses none of the extra syntax is just JavaScript with a
different extension.

```js
const runtime = typeof Sxn !== "undefined" ? "sxn " + Sxn.version : "something else";
console.log(`hello from ${runtime}`);
```

```sh
sxn hello.js
```

```
hello from sxn 0.0.1
```

TypeScript's erasable forms are all accepted: aliases, interfaces, `declare`,
annotations, optional parameters, generics on functions, `as`/`satisfies`, and
union types. `enum` and `namespace` are rejected on purpose rather than
stripped, because both emit a real object at runtime in TypeScript, and
quietly removing them would turn every use of their members into `undefined`:

```
SyntaxError: unsupported keyword: enum
```

## Precompiling

`sxn compile` writes bytecode that skips parsing on later runs:

```sh
sxn compile app.sx -o app.sxbc
sxn app.sxbc
```

`sxn --compile-cache app.sx` does the same thing automatically, building the
cache on the first launch and reusing it afterwards. The measured gains, and
the reason bytecode is not a safe format for untrusted input, are in
[the bytecode spec](../bytecode/).

## Where to go next

- [Examples](../examples/) — complete programs you can run, with their output.
- [The runtime surface](../runtime/) — `fetch`, `Sxn.serve`, streams, crypto, FFI.
- [Node compatibility](../node/) — what runs because it imitates Node.
- [The CLI](../cli/) — every command and flag.
