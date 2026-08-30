// Sxn.serve returns a handle, so a server can be stopped and a process can do
// something else afterwards instead of running until it is killed.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 8987 }, () => ({ statusCode: 200, body: "up" }));
check("returns a handle", typeof server, "object");
check("reports its port", server.port, 8987);
check("reports its url", server.url, "http://127.0.0.1:8987");
check("has stop", typeof server.stop, "function");

const res = await fetch(server.url + "/");
check("serves", await res.text(), "up");

server.stop();
let refused = false;
try { await fetch(server.url + "/"); } catch { refused = true; }
check("stopped", refused, true);
check("stop is idempotent", (() => { server.stop(); return "ok"; })(), "ok");

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
