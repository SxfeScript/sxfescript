// Where a server binds, and on what terms. The address used to be hardcoded
// to loopback with the option accepted and ignored, so a server could not be
// reached from another machine at all.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const loopback = Sxn.serve({ port: 0 }, () => new Response("loopback"));
check("loopback by default", loopback.hostname, "127.0.0.1");
check("url names the host", loopback.url, `http://127.0.0.1:${loopback.port}`);
check("serves", await (await fetch(loopback.url)).text(), "loopback");
loopback.stop();

const any = Sxn.serve({ port: 0, hostname: "0.0.0.0" }, () => new Response("any"));
check("binds where asked", any.hostname, "0.0.0.0");
check("reachable there", await (await fetch(`http://127.0.0.1:${any.port}/`)).text(), "any");
any.stop();

// Node calls it `host`; Bun and Deno call it `hostname`. Both work.
const named = Sxn.serve({ port: 0, host: "localhost" }, () => new Response("named"));
check("localhost resolves", named.hostname, "127.0.0.1");
check("and serves", await (await fetch(named.url)).text(), "named");
named.stop();

let threw = "";
try { Sxn.serve({ port: 0, hostname: "nope.invalid" }, () => new Response("x")); }
catch (e) { threw = e.message; }
check("a name it cannot bind is an error", /is not an IP address/.test(threw), true);

// reusePort is what lets several processes share a port and so use several
// cores. The kernels that distribute connections are Linux, the BSDs,
// Solaris and AIX; macOS says so rather than handing one process everything.
let reuse = "";
try { const s = Sxn.serve({ port: 0, reusePort: true }, () => new Response("r")); s.stop(); reuse = "ok"; }
catch (e) { reuse = /not supported on this platform/.test(e.message) ? "unsupported" : e.message; }
check("reusePort works or says why", reuse === "ok" || reuse === "unsupported", true);

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
