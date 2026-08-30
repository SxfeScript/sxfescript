// Top-level await must resume for work that only the event loop can finish,
// not just for promises that settle as microtasks.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

check("microtask", await Promise.resolve(42), 42);
check("timer", await new Promise(r => setTimeout(() => r(7), 20)), 7);
check("chained timers", await (async () => {
  let n = 0;
  for (let i = 0; i < 3; i++) n += await new Promise(r => setTimeout(() => r(i), 5));
  return n;
})(), 3);
const PORT = 8989;
Sxn.serve({ port: PORT }, () => ({ statusCode: 200, body: "pong" }));
const res = await fetch("http://127.0.0.1:" + PORT + "/");
check("fetch own server", res.status, 200);
check("fetch body", await res.text(), "pong");
check("rejection", await Promise.reject(new Error("x")).catch(e => e.message), "x");
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
