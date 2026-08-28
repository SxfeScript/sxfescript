# SXN network runtime

SXN installs browser-style `fetch` and a Bun-style `Sxn` host namespace in every
runtime context. `fetch(url, options)` and `Sxn.fetch(url, options)` use libcurl and support HTTP methods,
request bodies, redirects, status/ok fields, and `text()`/`json()` response
readers.

`__sxnServe(options, handler)` is the low-level server ABI used by ExpressX. It
`Sxn.serve(options, handler)` parses HTTP requests, invokes the handler, writes regular responses, emits SSE
event streams, and performs WebSocket upgrades and text frames.

Rayact's production desktop networking uses libwebsockets. SXN should use the
same library when its WebSocket client/event-loop API is added; the initial
server path deliberately keeps the ExpressX-facing ABI independent of the
underlying transport so that migration does not change application code.
