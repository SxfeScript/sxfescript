// Sxn.serve in Request/Response terms -- the shape spec/RUNTIME.md documents
// and the one a handler written for any other WinterTC runtime already uses.
// Regression for three things that each broke the documented example: a
// returned Response wrote a garbled reply, `new URL(req.url)` threw because
// req.url was a bare path, and `port: 0` reported 0 instead of the port the
// OS actually chose.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 0 }, async (req) => {
  const url = new URL(req.url);
  if (url.pathname === "/echo") return Response.json({ got: await req.json() }, { status: 201 });
  if (url.pathname === "/cookies")
    // The pairs form, because a record's value is a single string -- two
    // cookies joined by a comma are one malformed cookie, not two.
    return new Response("c", { headers: [["Set-Cookie", "a=1"], ["Set-Cookie", "b=2"]] });
  if (url.pathname === "/plain-object") return { statusCode: 202, body: "still works" };
  return new Response("hello " + url.searchParams.get("who"), {
    headers: { "X-Custom": "yes" },
  });
});

check("port 0 resolves", server.port > 0, true);
check("url carries that port", server.url, "http://127.0.0.1:" + server.port);

const a = await fetch(server.url + "/hi?who=world");
check("Response body", await a.text(), "hello world");
check("Response status", a.status, 200);
check("Response headers", a.headers.get("x-custom"), "yes");
check("string body keeps text/plain", a.headers.get("content-type"), "text/plain; charset=utf-8");

const b = await fetch(server.url + "/echo", { method: "POST", body: JSON.stringify({ n: 7 }) });
check("Response.json status", b.status, 201);
check("req.json()", await b.text(), '{"got":{"n":7}}');
check("Response.json content-type", b.headers.get("content-type"), "application/json");

const c = await fetch(server.url + "/cookies");
check("repeated Set-Cookie stays repeated", JSON.stringify(c.headers.getSetCookie()), '["a=1","b=2"]');

const d = await fetch(server.url + "/plain-object");
check("plain object still accepted", d.status, 202);
check("plain object body", await d.text(), "still works");

server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
