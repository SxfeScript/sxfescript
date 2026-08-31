// The request body is handed to JavaScript as the bytes the server already
// read, not copied into a string on the way. Everything that reads a body
// still has to work, and a pipelined request -- where the buffer cannot be
// handed over -- has to work too.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 0 }, async (req) => {
  const url = new URL(req.url);
  if (url.pathname === "/json") return Response.json({ n: (await req.json()).length });
  if (url.pathname === "/text") return new Response(await req.text());
  if (url.pathname === "/bytes") return Response.json({ bytes: (await req.arrayBuffer()).byteLength });
  if (url.pathname === "/twice") { const a = await req.text(); return new Response(String(a.length)); }
  if (url.pathname === "/ignored") return new Response("never read it");
  return new Response("?", { status: 404 });
});
const post = (path, body, type) => fetch(server.url + path, { method: "POST", headers: { "content-type": type ?? "application/json" }, body });

const big = JSON.stringify(Array.from({ length: 5000 }, (_, i) => ({ i })));
check("a big JSON body parses", (await (await post("/json", big)).json()).n, 5000);
check("and again on the same connection", (await (await post("/json", big)).json()).n, 5000);
check("text comes back whole", (await (await post("/text", "hello body", "text/plain")).text()), "hello body");
check("non-ASCII survives", (await (await post("/text", "héllo 🎉", "text/plain")).text()), "héllo 🎉");
check("bytes are the byte length", (await (await post("/bytes", "héllo", "text/plain")).json()).bytes, 6);
check("a body nobody reads", await (await post("/ignored", big)).text(), "never read it");
check("an empty body", await (await post("/text", "", "text/plain")).text(), "");
let threw = false;
try { await (await post("/json", "not json")).json(); } catch { threw = true; }
check("bad JSON is an error", threw, true);

server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
