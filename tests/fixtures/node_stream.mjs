import { Readable, Writable, Transform, PassThrough, pipeline, finished } from "node:stream";
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const collect = (r) => new Promise((res, rej) => {
  const out = []; r.on("data", (c) => out.push(c)); r.on("end", () => res(out)); r.on("error", rej);
});

p("Readable.from", await collect(Readable.from([1,2,3])));
p("async iteration", await (async () => { const o=[]; for await (const v of Readable.from(["a","b"])) o.push(v); return o; })());

// push/read in paused mode
{ const r = new Readable({ read(){} });
  r.push("x"); r.push("y"); r.push(null);
  p("paused read", [r.read(), r.read(), r.read()]); }

// pipe into a writable
{ const got = [];
  const w = new Writable({ objectMode: true, write(c, e, cb){ got.push(c); cb(); } });
  Readable.from(["p","q"]).pipe(w);
  await new Promise((res) => w.on("finish", res));
  p("pipe", got); }

// transform
{ const t = new Transform({ objectMode: true, transform(c, e, cb){ cb(null, c * 2); } });
  p("transform", await collect(Readable.from([1,2,3]).pipe(t))); }

// PassThrough
p("PassThrough", await collect(Readable.from(["z"]).pipe(new PassThrough())));

// pipeline with a callback
{ const got = [];
  await new Promise((res, rej) => pipeline(
    Readable.from([1,2]),
    new Transform({ objectMode: true, transform(c,e,cb){ cb(null, c + 10); } }),
    new Writable({ objectMode: true, write(c,e,cb){ got.push(c); cb(); } }),
    (err) => err ? rej(err) : res()));
  p("pipeline", got); }

// finished
{ const r = Readable.from(["one"]);
  r.resume();
  await new Promise((res) => finished(r, () => res()));
  p("finished", "called"); }

// errors surface
{ const w = new Writable({ objectMode: true, write(c,e,cb){ cb(new Error("bad write")); } });
  p("write error", await new Promise((res) => { w.on("error", (e) => res(e.message)); w.write("x"); })); }

// web bridges
p("toWeb", await (async () => {
  const web = Readable.toWeb(Readable.from(["w1","w2"]));
  const o = []; for await (const v of web) o.push(v); return o; })());
p("fromWeb", await collect(Readable.fromWeb(
  new ReadableStream({ start(c){ c.enqueue("f1"); c.enqueue("f2"); c.close(); } }))));
// Outside objectMode a stream refuses a non-byte chunk, as Node does.
p("rejects raw number", (() => { try { new Writable({ write(c,e,cb){cb();} }).write(5); return "no"; }
  catch (e) { return e.code; } })());
console.log(L.join("\n"));
