// The headers a served request carries are built on first read. Everything
// about them still has to behave as if they had been built up front.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 0 }, async (req) => {
  const url = new URL(req.url);
  if (url.pathname === "/read") return Response.json({
    ct: req.headers.get("content-type"),
    missing: req.headers.get("x-nope"),
    has: req.headers.has("x-marker"),
    twice: req.headers.get("x-marker") === req.headers.get("x-marker"),
    count: [...req.headers.keys()].length > 0,
  });
  if (url.pathname === "/ignore") return new Response("never read them");
  if (url.pathname === "/mutate") {
    req.headers.set("x-added", "1");
    return Response.json({ added: req.headers.get("x-added"), marker: req.headers.get("x-marker") });
  }
  if (url.pathname === "/clone") {
    const copy = req.clone();
    return Response.json({ same: copy.headers.get("x-marker") });
  }
  if (url.pathname === "/body") return Response.json({ body: await req.json() });
  return new Response("?", { status: 404 });
});

const call = (path, init) => fetch(server.url + path, init);
const headers = { "x-marker": "here", "content-type": "application/json" };

const read = await (await call("/read", { headers })).json();
check("reads a header", read.ct, "application/json");
check("a missing header is null", read.missing, null);
check("has() works", read.has, true);
check("reading twice is stable", read.twice, true);
check("iterates", read.count, true);
check("a handler that ignores them still works", await (await call("/ignore", { headers })).text(), "never read them");
const mutated = await (await call("/mutate", { headers })).json();
check("can be added to", mutated.added, "1");
check("and keeps what arrived", mutated.marker, "here");
check("clone carries them", (await (await call("/clone", { headers })).json()).same, "here");
check("a body still reads", (await (await call("/body", { method: "POST", headers, body: '{"n":1}' })).json()).body.n, 1);

server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
