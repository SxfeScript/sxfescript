// A request body arrives over as many reads as the kernel gives us. Until the
// server accumulated them, anything past one read (about 64KB) reached the
// handler truncated, so a 1MB JSON POST -- an ordinary API request -- failed
// to parse.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 0 }, async (req) => {
  const body = await req.text();
  return Response.json({ length: body.length, first: body.slice(0, 4), last: body.slice(-4) });
});

for (const size of [1024, 65536, 1024 * 1024, 4 * 1024 * 1024]) {
  const body = "ab" + "x".repeat(size - 4) + "yz";
  const res = await fetch(server.url + "/", { method: "POST", headers: { "content-type": "text/plain" }, body });
  const got = await res.json();
  check(`${size} bytes arrive whole`, got.length, body.length);
  check(`${size} bytes are intact`, got.first + got.last, "abxxxxyz");
}

// A body may contain a NUL byte; its length must not come from strlen.
const withNul = await fetch(server.url + "/", { method: "POST", body: "a\0b" });
check("a NUL byte does not truncate", (await withNul.json()).length, 3);

server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
