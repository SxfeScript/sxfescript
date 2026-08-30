# Examples

Every program on this page is a real file in
[`examples/`](https://github.com/SxfeScript/sxfescript/tree/main/examples), and
the output under each one is what it actually prints. Clone the repo and run
them, or paste one into a file and run that.

## Types and borrows

[`examples/hello.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/hello.sx)

```sx
interface Repo {
  name: string;
  stars: i32;
}

const describe = (repo: Repo): string =>
  `${repo.name} has ${repo.stars} star${repo.stars === 1 ? "" : "s"}`;

console.log(describe({ name: "sxfescript", stars: 1 }));

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
sxn examples/hello.sx
```

```
sxfescript has 1 star
counter: 2
```

## A fixed-layout struct

[`examples/velocity.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/velocity.sx)

An interface whose fields are all primitives (`i32`, `f32`, `f64`, `bool`)
describes a struct with declared field order and natural alignment — the same
layout on every supported target, which is what code crossing into native
memory needs.

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

```sh
sxn examples/velocity.sx
```

```
{"x":0.016,"y":10,"z":5}
```

## An HTTP server

[`examples/server.mjs`](https://github.com/SxfeScript/sxfescript/blob/main/examples/server.mjs)

The handler receives a `Request` and returns a `Response`. `req.url` is
absolute, so `new URL(req.url)` gives you the path and query, and
`await req.json()` reads the body.

```js
const notes = new Map([[1, "the first note"]]);
let nextId = 2;

const server = Sxn.serve({ port: 0 }, async (req) => {
  const url = new URL(req.url);

  if (url.pathname === "/") {
    return new Response("try /notes");
  }

  if (url.pathname === "/notes" && req.method === "GET") {
    return Response.json([...notes].map(([id, text]) => ({ id, text })));
  }

  if (url.pathname === "/notes" && req.method === "POST") {
    const { text } = await req.json();
    const id = nextId++;
    notes.set(id, text);
    return Response.json({ id, text }, { status: 201 });
  }

  return new Response("not found", { status: 404 });
});

console.log(`listening on ${server.url}`);

const created = await fetch(`${server.url}/notes`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ text: "written by the example" }),
});
console.log("POST /notes ->", created.status, await created.text());

const all = await fetch(`${server.url}/notes`);
console.log("GET  /notes ->", all.status, await all.text());

server.stop();
```

```sh
sxn examples/server.mjs
```

```
listening on http://127.0.0.1:56690
POST /notes -> 201 {"id":2,"text":"written by the example"}
GET  /notes -> 200 [{"id":1,"text":"the first note"},{"id":2,"text":"written by the example"}]
```

The port differs every run, because `port: 0` asks the operating system to
pick a free one. Pass a real port number to choose it yourself.

## fetch and streams

[`examples/fetch.mjs`](https://github.com/SxfeScript/sxfescript/blob/main/examples/fetch.mjs)

A response body is a real `ReadableStream`, so it can be piped and consumed a
chunk at a time rather than only read whole.

```js
const res = await fetch("https://example.com/");
console.log(res.status, res.headers.get("content-type"));

const html = await res.text();
console.log(`${html.length} bytes`);

const streamed = await fetch("https://example.com/");
let chunks = 0;
let characters = 0;
for await (const chunk of streamed.body.pipeThrough(new TextDecoderStream())) {
  chunks += 1;
  characters += chunk.length;
}
console.log(`${chunks} chunk(s), ${characters} characters`);
```

```sh
sxn examples/fetch.mjs
```

```
200 text/html
559 bytes
1 chunk(s), 559 characters
```

## Files

[`examples/files.mjs`](https://github.com/SxfeScript/sxfescript/blob/main/examples/files.mjs)

`Sxn.file` and `Sxn.write` are the runtime's own file I/O. `node:fs` works
too, over the same files, for code that already expects it.

```js
import { tmpdir } from "node:os";
import { join } from "node:path";
import { readFile } from "node:fs/promises";

const path = join(tmpdir(), "sxn-example.txt");

await Sxn.write(path, "written by the example\n");

const file = Sxn.file(path);
console.log(JSON.stringify(await file.text()));

console.log("via node:fs ->", JSON.stringify(await readFile(path, "utf8")));

console.log("sxn version:", Sxn.version);
```

```sh
sxn examples/files.mjs
```

```
"written by the example\n"
via node:fs -> "written by the example\n"
sxn version: 0.0.1
```

## Calling a C function

[`examples/ffi.mjs`](https://github.com/SxfeScript/sxfescript/blob/main/examples/ffi.mjs)

`Sxn.ffi(library, symbol, argumentTypes, returnType)` returns a callable
JavaScript function, through libffi and `dlopen`.

```js
const libm = {
  darwin: "libSystem.B.dylib",
  linux: "libm.so.6",
  win32: "msvcrt.dll",
}[process.platform];

const pow = Sxn.ffi(libm, "pow", ["f64", "f64"], "f64");
const sqrt = Sxn.ffi(libm, "sqrt", ["f64"], "f64");

console.log("pow(2, 10) =", pow(2, 10));
console.log("sqrt(144)  =", sqrt(144));
```

```sh
sxn examples/ffi.mjs
```

```
pow(2, 10) = 1024
sqrt(144)  = 12
```

Structs by value, callbacks and variadics are rejected rather than
half-supported. [The native-code spec](../native/) has the full type list and
the reasoning.
