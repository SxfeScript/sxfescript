# SXN network runtime

The server, fetch, and Web Streams surface described here is documented in
full, with examples, in `spec/RUNTIME.md`. This page stays as the pointer to
it and the one implementation detail worth knowing separately: the transport.

`fetch`/`Sxn.fetch` are backed by libcurl. `Sxn.serve` is a native HTTP
server with its own event loop integration (`sxn_loop()` in `src/network.c`,
a thin wrapper over `uv_default_loop()`) rather than a JS-level framework
sitting on top of sockets -- request parsing, response writing, SSE framing,
and the WebSocket upgrade handshake all happen in C.

Rayact's own production networking uses libwebsockets. If SXN grows an
outbound WebSocket client, that's the library to match, so a future migration
of the underlying transport doesn't change application code -- the server-side
upgrade path already keeps its ABI independent of the transport for the same
reason.
