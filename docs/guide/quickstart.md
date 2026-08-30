# Quick start

`sxn` is a single binary. It runs `.sx`, `.ts`, `.js`, `.mjs` and `.cjs`
files directly, with no build step and nothing to configure first.

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

Ordinary JavaScript runs as-is. Put this in `hello.js`:

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

## TypeScript, with no build step

Rename it to `hello.ts` and add annotations. `sxn` parses and strips them
itself, so there is no `tsc` and no bundler in front of it:

```ts
interface Runtime {
  name: string;
  version: string;
}

function describe(r: Runtime): string {
  return `hello from ${r.name} ${r.version}`;
}

console.log(describe({ name: "sxn", version: Sxn.version }));
```

```sh
sxn hello.ts
```

```
hello from sxn 0.0.1
```

Types that can be erased are accepted: aliases, interfaces, `declare`,
annotations, optional parameters, generics on functions, `as`/`satisfies`,
and union types. `enum` and `namespace` are rejected on purpose rather than
stripped, because both emit a real object at runtime in TypeScript, and
quietly removing them would turn every use of their members into `undefined`:

```
SyntaxError: unsupported keyword: enum
```

## `.sx`: ownership and borrows

A `.sx` file is the same language with mutation and aliasing made explicit.
`let mut` is a mutable owner, `let` an immutable one, `&` borrows a value
shared, and `&mut` borrows it exclusively:

```ts
interface Counter {
  hits: i32;
}

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

The syntax is parsed natively today. The full control-flow ownership pass
that enforces every rule in
[the language contract](../language/) is still being written —
[the implementation ledger](../implementation/) tracks exactly what is
checked and what is only parsed, and it is worth reading before you rely on a
rule being enforced.

## An HTTP server

`Sxn.serve` hands your function a `Request` and expects a `Response` back —
the same pair of objects a handler gets on Cloudflare Workers, Deno or Bun:

```js
const server = Sxn.serve({ port: 3000 }, async (req) => {
  const url = new URL(req.url);
  if (url.pathname === "/echo") return Response.json(await req.json());
  return new Response("hello from " + url.pathname);
});

console.log(`listening on ${server.url}`);
```

```sh
sxn server.js
```

```
listening on http://127.0.0.1:3000
```

`port: 0` asks the operating system for a free port instead, and
`server.port` then tells you which one it picked. `server.stop()` shuts the
listener down, so one process can serve and then go on to do something else.

## Precompiling

`sxn compile` writes bytecode that skips parsing on later runs:

```sh
sxn compile app.js -o app.sxbc
sxn app.sxbc
```

`sxn --compile-cache app.js` does the same thing automatically, building the
cache on the first launch and reusing it afterwards. The measured gains, and
the reason bytecode is not a safe format for untrusted input, are in
[the bytecode spec](../bytecode/).

## Where to go next

- [Examples](../examples/) — complete programs you can run, with their output.
- [The runtime surface](../runtime/) — `fetch`, `Sxn.serve`, streams, crypto, FFI.
- [Node compatibility](../node/) — what runs because it imitates Node.
- [The CLI](../cli/) — every command and flag.
