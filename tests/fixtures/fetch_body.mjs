// Sending a request body: POST, PUT and PATCH, JSON, and a body that is
// echoed back unchanged. Regression for POSTFIELDSIZE ordering, which made
// every method with a body connect and then send nothing.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const server = Sxn.serve({ port: 8973 }, (req) => ({
  statusCode: 200,
  headers: { "content-type": "text/plain" },
  body: (req.method || "?") + ":" + (req.body || ""),
}));

check("POST", await (await fetch(server.url + "/", { method: "POST", body: "one" })).text(), "POST:one");
check("PUT", await (await fetch(server.url + "/", { method: "PUT", body: "two" })).text(), "PUT:two");
check("PATCH", await (await fetch(server.url + "/", { method: "PATCH", body: "three" })).text(), "PATCH:three");
check("GET has no body", await (await fetch(server.url + "/")).text(), "GET:");
check("empty body", await (await fetch(server.url + "/", { method: "POST", body: "" })).text(), "POST:");

const json = JSON.stringify({ a: 1, b: "x" });
check("json body", await (await fetch(server.url + "/", {
  method: "POST", headers: { "content-type": "application/json" }, body: json,
})).text(), "POST:" + json);

const big = "x".repeat(5000);
check("large body", await (await fetch(server.url + "/", { method: "POST", body: big })).text(), "POST:" + big);

const req = new Request(server.url + "/", { method: "POST", body: "via-request" });
check("Request object body", await (await fetch(req)).text(), "POST:via-request");

server.stop();
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
