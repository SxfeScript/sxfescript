# Examples

Every program on this page is a real file in
[`examples/`](https://github.com/SxfeScript/sxfescript/tree/main/examples), and
the output under each one is what it actually prints. The code below is
inlined from those files when this page is built, so it cannot drift out of
step with them. Clone the repo and run them, or paste one into a file and run
that.

They are all `.sx`, because that is the language this project is for.
Everything a `.sx` file can do here, a plain `.js`, `.mjs` or `.ts` file can
do too — the annotations and the ownership syntax are the only difference, and
[the quick start](../quickstart/) shows the same program in each.

## Types and borrows

[`examples/hello.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/hello.sx)

<!-- include: examples/hello.sx as sx -->

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
memory needs. It is also the one shape `sxn` does not allocate: the first pair
of vectors below never becomes an object at all. The second pair does, and the
difference is the borrow — a callee that may write through a value is a callee
that needs one to write to.

<!-- include: examples/velocity.sx as sx -->

```sh
sxn examples/velocity.sx
```

```
{"x":0.016,"y":10,"z":5}
{"x":0.016,"y":10,"z":5}
```

## An HTTP server

[`examples/server.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/server.sx)

The handler receives a `Request` and returns a `Response`. `req.url` is
absolute, so `new URL(req.url)` gives you the path and query, and
`await req.json()` reads the body.

<!-- include: examples/server.sx as sx -->

```sh
sxn examples/server.sx
```

```
listening on http://127.0.0.1:56690
POST /notes -> 201 {"id":2,"text":"written by the example"}
GET  /notes -> 200 [{"id":1,"text":"the first note"},{"id":2,"text":"written by the example"}]
```

The port differs every run, because `port: 0` asks the operating system to
pick a free one. Pass a real port number to choose it yourself.

## fetch and streams

[`examples/fetch.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/fetch.sx)

A response body is a real `ReadableStream`, so it can be piped and consumed a
chunk at a time rather than only read whole.

<!-- include: examples/fetch.sx as sx -->

```sh
sxn examples/fetch.sx
```

```
200 text/html
559 bytes
1 chunk(s), 559 characters
```

## Files

[`examples/files.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/files.sx)

`Sxn.file` and `Sxn.write` are the runtime's own file I/O. The `node:fs`
import is the compatibility layer reading the same file back, for code that
already expects Node.

<!-- include: examples/files.sx as sx -->

```sh
sxn examples/files.sx
```

```
"written by the example\n"
via node:fs -> "written by the example\n"
sxn version: 0.0.2
```

## Calling a C function

[`examples/ffi.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/ffi.sx)

`Sxn.ffi(library, symbol, argumentTypes, returnType)` returns a callable
function, through libffi and `dlopen`.

<!-- include: examples/ffi.sx as sx -->

```sh
sxn examples/ffi.sx
```

```
pow(2, 10) = 1024
sqrt(144)  = 12
```

Structs by value, callbacks and variadics are rejected rather than
half-supported. [The native-code spec](../native/) has the full type list and
the reasoning.
