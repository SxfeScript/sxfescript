const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// structuredClone
const src = { n: 1, s: "x", d: new Date(0), r: /ab/gi, arr: [1,[2]],
              m: new Map([["k",{v:1}]]), set: new Set([1,2]),
              ta: new Uint8Array([1,2,3]), ab: new ArrayBuffer(4), nested: { deep: true } };
src.self = src;
const c = structuredClone(src);
p("clone independent", c !== src && c.nested !== src.nested);
p("clone values", [c.n, c.s, c.d instanceof Date && c.d.getTime(), c.r.source, c.r.flags, c.arr[1][0]]);
p("clone map", [c.m instanceof Map, c.m.get("k").v, c.m.get("k") !== src.m.get("k")]);
p("clone set", [c.set instanceof Set, [...c.set]]);
p("clone typed", [Array.from(c.ta), c.ta !== src.ta]);
p("clone arraybuffer", [c.ab.byteLength, c.ab !== src.ab]);
p("clone cycle", c.self === c);
p("clone rejects fn", (()=>{try{structuredClone(()=>{});return "no"}catch(e){return e.name}})());
// URL
p("canParse ok", URL.canParse("https://a.example/x"));
p("canParse bad", URL.canParse("::::"));
p("canParse relative+base", URL.canParse("/p", "https://a.example"));
// crypto.randomUUID
const u = crypto.randomUUID();
p("uuid shape", /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u));
p("uuid unique", crypto.randomUUID() !== crypto.randomUUID());
// Response statics
const rj = Response.json({ a: 1 });
p("Response.json", [rj.status, rj.headers.get("content-type"), await rj.text()]);
const rr = Response.redirect("https://a.example/z", 301);
p("Response.redirect", [rr.status, rr.headers.get("location")]);
p("redirect bad status", (()=>{try{Response.redirect("https://a.example",200);return "no"}catch(e){return e.constructor.name}})());
// clone
const r1 = new Response("body-text", { status: 201, headers: { "x-a": "1" } });
const r2 = r1.clone();
p("Response.clone", [r2.status, r2.headers.get("x-a"), await r2.text(), await r1.text()]);
const q1 = new Request("https://a.example/p", { method: "POST" });
p("Request.clone", [q1.clone().url, q1.clone().method]);
// Headers.getSetCookie
const h = new Headers([["set-cookie","a=1"],["set-cookie","b=2"],["x","y"]]);
p("getSetCookie", h.getSetCookie());
p("getSetCookie empty", new Headers().getSetCookie());
console.log(L.join("\n"));
