// Sxn.serve response headers: a JSON API needs Content-Type, cookies and
// cache directives, and must not let a handler corrupt the framing headers
// that describe this particular response.
let bad = 0;
const check = (name, got, want) => {
  const ok = got === want;
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + name + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want));
};

const PORT = 8991;
Sxn.serve({ port: PORT }, (req) => {
  const path = new URL(req.url).pathname;
  if (path === "/plain") return { statusCode: 200, body: "hi" };
  return {
    statusCode: 201,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "X-Custom": "yes",
      "Set-Cookie": ["a=1; Path=/", "b=2; HttpOnly"],
      "Content-Length": "999",     // must be ignored: we frame the response
      "Connection": "keep-alive",  // likewise
      "X-Skipped": undefined,      // absent values are not emitted
    },
    body: '{"ok":true}',
  };
});

const res = await fetch("http://127.0.0.1:" + PORT + "/api");
check("status", res.status, 201);
check("content-type honored", res.headers.get("content-type"), "application/json; charset=utf-8");
check("custom header", res.headers.get("x-custom"), "yes");
check("cache-control absent", res.headers.get("cache-control"), null);
check("undefined skipped", res.headers.get("x-skipped"), null);
check("content-length is ours", res.headers.get("content-length"), "11");
check("body intact", await res.text(), '{"ok":true}');

const plain = await fetch("http://127.0.0.1:" + PORT + "/plain");
check("default content-type", plain.headers.get("content-type"), "text/plain; charset=utf-8");
check("plain body", await plain.text(), "hi");

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
// Sxn.serve returns no handle, so the listening socket keeps the loop
// alive; exiting is the only way to end the test.
process.exit(bad === 0 ? 0 : 1);
