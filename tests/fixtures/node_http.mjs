import http from "node:http";
import { once } from "node:events";
let bad = 0;
const check = (n, got, want) => { const ok = JSON.stringify(got) === JSON.stringify(want); if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + (ok ? "" : " want=" + JSON.stringify(want))); };

const server = http.createServer((req, res) => {
  const url = req.url;
  if (url === "/read-body") {
    // The body is pushed on the first read, never before, and 'end' marks
    // the request complete -- both of which are shared native functions now
    // rather than closures built per request.
    let n = 0;
    const early = req.complete;
    req.on("data", (c) => { n += c.length; });
    req.on("end", () => res.end(JSON.stringify({ n, early, complete: req.complete })));
  } else if (url === "/socket") {
    // The socket a request carries: the fields on-finished and finalhandler
    // read, the setters they chain off, and destroy() emitting 'close'.
    const sock = req.socket;
    const before = { readable: sock.readable, writable: sock.writable, destroyed: sock.destroyed,
                     addr: typeof sock.remoteAddress, sameAsConnection: req.connection === sock,
                     chains: sock.setNoDelay(true) === sock && sock.setKeepAlive(true) === sock &&
                             sock.setTimeout(0) === sock };
    // destroy() is not exercised here: under Node it really does close the
    // connection, and this response still has to reach the client.
    res.end(JSON.stringify(before));
  } else if (url === "/body-shapes") {
    // How the written chunks are joined into one body: one string, several
    // strings, bytes only, and a mix of the two.
    if (req.headers["x-shape"] === "multi") { res.write("a"); res.write("b"); res.end("c"); }
    else if (req.headers["x-shape"] === "mixed") { res.write("a"); res.end(new Uint8Array([66, 67])); }
    else if (req.headers["x-shape"] === "bytes") { res.write(new Uint8Array([65])); res.end(new Uint8Array([66])); }
    else if (req.headers["x-shape"] === "empty") res.end();
    else res.end("one");
  } else if (url === "/plumbing") {
    // The shapes finalhandler and on-finished reach for when they answer a
    // request nobody read: unpipe on a stream that was never piped, and a
    // socket they can subscribe to.
    const seen = {
      unpipe: typeof req.unpipe === "function",
      socketOn: typeof req.socket.on === "function",
      socketWritable: req.socket.writable === true,
    };
    req.unpipe();
    let heard = false;
    req.socket.on("sxn-probe", () => { heard = true; });
    req.socket.emit("sxn-probe");
    req.resume();
    res.setHeader("content-type", "application/json");
    res.end(JSON.stringify({ ...seen, heard }));
  } else if (url === "/late-body") {
    // on-finished and body-parser both decide from these, and a body pushed
    // before the consumer subscribes would arrive as an empty string.
    const seen = { complete: req.complete, sockReadable: !!req.socket.readable };
    setTimeout(() => {
      let body = "";
      req.on("data", (c) => { body += c; });
      req.on("end", () => {
        res.setHeader("content-type", "application/json");
        res.end(JSON.stringify({ ...seen, body, doneAfter: req.complete }));
      });
    }, 0);
  } else if (url === "/json") {
    res.writeHead(201, { "content-type": "application/json", "x-a": "1" });
    // Header names arrive lowercased whatever the client sent, and
    // rawHeaders is the same list flattened into name, value pairs.
    // Node keeps the original spelling in rawHeaders, so the pairs are
    // matched against req.headers case-insensitively.
    let rawOk = req.rawHeaders.length === Object.keys(req.headers).length * 2;
    for (let i = 0; rawOk && i < req.rawHeaders.length; i += 2)
      rawOk = req.headers[req.rawHeaders[i].toLowerCase()] === req.rawHeaders[i + 1];
    res.end(JSON.stringify({ method: req.method, ua: !!req.headers["user-agent"],
                             xcase: req.headers["x-mixed-case"], rawOk }));
  } else if (url === "/chunks") {
    res.setHeader("content-type", "text/plain");
    res.write("one ");
    res.write("two");
    res.end();
  } else if (url === "/echo") {
    let body = "";
    req.on("data", (c) => { body += c; });
    req.on("end", () => { res.statusCode = 200; res.end("echo:" + body); });
  } else if (url === "/headers") {
    res.setHeader("x-set", "yes");
    check("hasHeader", res.hasHeader("x-set"), true);
    check("getHeader", res.getHeader("X-Set"), "yes");
    check("getHeaderNames", res.getHeaderNames(), ["x-set"]);
    // The name is lowercased whatever spelling it arrives in, and hasHeader
    // asks about own properties only.
    res.setHeader("X-Mixed", "1");
    check("mixed case set", res.getHeader("x-mixed"), "1");
    check("hasHeader is own only", res.hasHeader("constructor"), false);
    check("missing header", res.getHeader("x-absent"), undefined);
    check("setHeader chains", res.setHeader("x-chain", "c") === res, true);
    res.removeHeader("x-set");
    res.removeHeader("X-Mixed");
    res.removeHeader("x-chain");
    check("removeHeader", res.hasHeader("x-set"), false);
    check("removeHeader mixed case", res.getHeaderNames(), []);
    res.end("hdr");
  } else if (url === "/late") {
    setTimeout(() => res.end("after a tick"), 20);
  } else {
    res.statusCode = 404;
    res.end("not found");
  }
});
server.listen(8961);
await once(server, "listening");
check("address port", server.address().port, 8961);

const base = "http://127.0.0.1:8961";
const j = await fetch(base + "/json", { headers: { "X-Mixed-Case": "kept" } });
check("status", j.status, 201);
check("content-type", j.headers.get("content-type"), "application/json");
check("custom header", j.headers.get("x-a"), "1");
check("json body", await j.json(), { method: "GET", ua: true, xcase: "kept", rawOk: true });

const c = await fetch(base + "/chunks");
check("multiple writes", await c.text(), "one two");

const e = await fetch(base + "/echo", { method: "POST", body: "payload" });
check("request body read", await e.text(), "echo:payload");

check("header api", (await fetch(base + "/headers")).status, 200);

const late = await fetch(base + "/late");
check("async response", await late.text(), "after a tick");

const nf = await fetch(base + "/nope");
check("404", [nf.status, await nf.text()], [404, "not found"]);

const lb = await (await fetch(base + "/late-body", { method: "POST", body: "deferred" })).json();
check("body survives a late listener", lb,
      { complete: false, sockReadable: true, body: "deferred", doneAfter: true });

check("read body", await (await fetch(base + "/read-body", { method: "POST", body: "hello body" })).json(),
      { n: 10, early: false, complete: true });
check("read empty body", await (await fetch(base + "/read-body")).json(),
      { n: 0, early: false, complete: true });

check("socket", await (await fetch(base + "/socket")).json(),
      { readable: true, writable: true, destroyed: false, addr: "string",
        sameAsConnection: true, chains: true });

for (const [shape, want] of [["one", "one"], ["multi", "abc"], ["mixed", "aBC"], ["bytes", "AB"], ["empty", ""]])
  check("body " + shape, await (await fetch(base + "/body-shapes", { headers: { "x-shape": shape } })).text(), want);

const pl = await (await fetch(base + "/plumbing")).json();
check("request plumbing", pl,
      { unpipe: true, socketOn: true, socketWritable: true, heard: true });

check("STATUS_CODES", http.STATUS_CODES[404], "Not Found");
check("METHODS has POST", http.METHODS.includes("POST"), true);

await new Promise((r) => server.close(r));
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
