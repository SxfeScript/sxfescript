# An HTTP server

`Sxn.serve` hands your function a `Request` and expects a `Response` back — the
same pair of objects a handler gets on Cloudflare Workers, Deno or Bun. There is
no framework to install and nothing to configure.

```sx
const server = Sxn.serve({ port: 3000 }, (req: Request): Response => {
  return new Response("hello");
});

console.log(`listening on ${server.url}`);
```

```sh
sxn server.sx
```

```
listening on http://127.0.0.1:3000
```

`port: 0` asks the operating system for a free port instead, and `server.port`
tells you which one it picked. `server.stop()` shuts the listener down, so one
process can serve and then go on to do something else — without it, the
listening socket keeps the process alive, which is what you want for a real
server and not for a script that has finished.

## Reading the request

`req.url` is absolute, so `new URL(req.url)` gives you the path and the query.
`req.method` and `req.headers` are what you would expect. The body is read with
`req.text()`, `req.json()` or `req.arrayBuffer()`, all of which return promises,
so a handler that touches the body is `async`.

```sx
const server = Sxn.serve({ port: 0 }, async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  if (url.pathname === "/echo" && req.method === "POST") {
    return Response.json(await req.json());
  }
  return new Response("try POST /echo", { status: 404 });
});

const r = await fetch(`${server.url}/echo`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ hello: "world" }),
});
console.log(r.status, await r.text());
server.stop();
```

```
200 {"hello":"world"}
```

A request larger than 64 MB is refused rather than buffered, because the whole
body is held in memory before your handler sees it.

## What a handler can return

- A `Response`, including `Response.json(value, init)`.
- A promise for one — the connection waits, and other connections are served
  meanwhile.
- A plain `{ statusCode, headers, body }` object. This is the shape the native
  layer speaks, and it is what `node:http` is built on directly, so returning
  it skips one layer.
- A WebSocket upgrade, or `Sxn.serve`'s own server-sent-events helper.

Keep-alive is on by default: a connection survives its response, so a client
doing a hundred requests opens one socket rather than a hundred. A client that
sends `Connection: close` gets that instead.

## A complete program

This is [`examples/server.sx`](https://github.com/SxfeScript/sxfescript/blob/main/examples/server.sx)
in the repo, and it runs as-is:

<!-- include: examples/server.sx as sx -->

```sh
sxn examples/server.sx
```

```
listening on http://127.0.0.1:56690
POST /notes -> 201 {"id":2,"text":"written by the example"}
GET  /notes -> 200 [{"id":1,"text":"the first note"},{"id":2,"text":"written by the example"}]
```

## Using `node:http` instead

If you are porting code that already speaks Node's API, `node:http` works and
is built on the same native layer:

```js
import { createServer } from "node:http";

createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/plain" });
  res.end("hello");
}).listen(3000);
```

[Node compatibility](../node/) has the details of what is and is not there.

## What to read next

- [The runtime surface](../runtime/) — `fetch`, streams, crypto, and the rest
  of the `Sxn` namespace.
- [Examples](../examples/) — more complete programs.
- [Networking](../network/) — the primitives underneath.
