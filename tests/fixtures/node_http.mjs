import http from "node:http";
import { once } from "node:events";
let bad = 0;
const check = (n, got, want) => { const ok = JSON.stringify(got) === JSON.stringify(want); if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + (ok ? "" : " want=" + JSON.stringify(want))); };

const server = http.createServer((req, res) => {
  const url = req.url;
  if (url === "/late-body") {
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
    res.end(JSON.stringify({ method: req.method, ua: !!req.headers["user-agent"] }));
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
    res.removeHeader("x-set");
    check("removeHeader", res.hasHeader("x-set"), false);
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
const j = await fetch(base + "/json");
check("status", j.status, 201);
check("content-type", j.headers.get("content-type"), "application/json");
check("custom header", j.headers.get("x-a"), "1");
check("json body", await j.json(), { method: "GET", ua: true });

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

check("STATUS_CODES", http.STATUS_CODES[404], "Not Found");
check("METHODS has POST", http.METHODS.includes("POST"), true);

await new Promise((r) => server.close(r));
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
