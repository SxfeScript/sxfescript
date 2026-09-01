// A connection survives its response. Until it did, every request paid for a
// new TCP connection: the server answered `Connection: close` and hung up, so
// a client doing more than a handful of requests ran out of ephemeral ports
// before the server ran out of capacity.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 0 }, () => new Response("hi"));

const first = await fetch(server.url + "/");
check("keeps the connection", first.headers.get("connection"), "keep-alive");
check("frames the body", first.headers.get("content-length"), "2");
check("answers", await first.text(), "hi");

// The same connection, reused: 50 requests in a row must all be answered.
let ok = 0;
for (let i = 0; i < 50; i++) if ((await fetch(server.url + "/")).status === 200) ok++;
check("serves request after request", ok, 50);

// A client that asks for the connection to close gets that instead.
const closed = await fetch(server.url + "/", { headers: { connection: "close" } });
check("honors Connection: close", closed.headers.get("connection"), "close");

// stop() has to close live connections too, or the loop never drains.
server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
