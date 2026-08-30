# Examples

Every program on this page is a real file in
[`examples/`](https://github.com/SxfeScript/sxfescript/tree/main/examples), and
the output under each one is what it actually prints. The code below is
inlined from those files when this page is built, so it cannot drift out of
step with them. Clone the repo and run them, or paste one into a file and run
that.

## Types and borrows

[`examples/hello.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/hello.sx)

<!-- include: examples/hello.sx as ts -->

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

<!-- include: examples/velocity.sx as ts -->

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

<!-- include: examples/server.mjs as js -->

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

<!-- include: examples/fetch.mjs as js -->

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

<!-- include: examples/files.mjs as js -->

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

<!-- include: examples/ffi.mjs as js -->

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
