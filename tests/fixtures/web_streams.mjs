const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));

// --- ReadableStream: reading, closing, done ---
{ const s = new ReadableStream({ start(c){ c.enqueue("a"); c.enqueue("b"); c.close(); } });
  const r = s.getReader(); const out = [];
  for (;;) { const { value, done } = await r.read(); if (done) break; out.push(value); }
  p("read to close", out); }

// --- locking ---
{ const s = new ReadableStream({ start(c){ c.close(); } });
  p("unlocked", s.locked);
  const r = s.getReader();
  p("locked", s.locked);
  p("double lock throws", (()=>{ try { s.getReader(); return "no" } catch(e){ return e.constructor.name } })());
  r.releaseLock();
  p("unlocked after release", s.locked); }

// --- pull-driven source and backpressure ---
{ let pulls = 0;
  const s = new ReadableStream({
    pull(c) { pulls++; c.enqueue(pulls); if (pulls === 3) c.close(); },
  }, { highWaterMark: 1 });
  const out = []; const r = s.getReader();
  for (;;) { const { value, done } = await r.read(); if (done) break; out.push(value); }
  p("pull source", out); }

// --- errors propagate ---
{ const s = new ReadableStream({ start(c){ c.error(new Error("boom")); } });
  p("error", await s.getReader().read().then(()=> "no", e => e.message)); }

// --- async iteration ---
{ const s = new ReadableStream({ start(c){ c.enqueue(1); c.enqueue(2); c.enqueue(3); c.close(); } });
  const out = []; for await (const v of s) out.push(v);
  p("async iteration", out); }

// --- from an iterable ---
if (typeof ReadableStream.from === "function") {
  const out = []; for await (const v of ReadableStream.from([1,2,3])) out.push(v);
  p("ReadableStream.from", out);
}

// --- tee ---
{ const s = new ReadableStream({ start(c){ c.enqueue("x"); c.enqueue("y"); c.close(); } });
  const [a, b] = s.tee();
  const collect = async (st) => { const o = []; for await (const v of st) o.push(v); return o; };
  p("tee", await Promise.all([collect(a), collect(b)])); }

// --- cancel ---
{ let reason = null;
  const s = new ReadableStream({ cancel(r){ reason = r; } });
  await s.cancel("done here");
  p("cancel reason", reason); }

// --- WritableStream ---
{ const written = [];
  const w = new WritableStream({ write(chunk){ written.push(chunk); } });
  const wr = w.getWriter();
  await wr.write("one"); await wr.write("two"); await wr.close();
  p("writable", written);
  p("closed resolves", await wr.closed.then(()=> "closed")); }

// --- writable errors ---
{ const w = new WritableStream({ write(){ throw new Error("nope"); } });
  const wr = w.getWriter();
  p("write error", await wr.write("x").then(()=> "no", e => e.message)); }

// --- TransformStream, identity and mapping ---
{ const t = new TransformStream({ transform(chunk, c){ c.enqueue(chunk * 2); } });
  const wr = t.writable.getWriter();
  (async () => { await wr.write(1); await wr.write(2); await wr.close(); })();
  const out = []; for await (const v of t.readable) out.push(v);
  p("transform", out); }

// --- pipeThrough / pipeTo ---
{ const src = new ReadableStream({ start(c){ c.enqueue(1); c.enqueue(2); c.close(); } });
  const doubler = new TransformStream({ transform(v, c){ c.enqueue(v * 10); } });
  const got = [];
  await src.pipeThrough(doubler).pipeTo(new WritableStream({ write(v){ got.push(v); } }));
  p("pipeThrough+pipeTo", got); }

// --- queuing strategies ---
p("CountQueuingStrategy", (() => { const q = new CountQueuingStrategy({ highWaterMark: 3 });
  return [q.highWaterMark, q.size()]; })());
p("ByteLengthQueuingStrategy", (() => { const q = new ByteLengthQueuingStrategy({ highWaterMark: 8 });
  return [q.highWaterMark, q.size(new Uint8Array(5))]; })());

// --- text transforms ---
{ const enc = new TextEncoderStream();
  const wr = enc.writable.getWriter();
  (async () => { await wr.write("héllo"); await wr.close(); })();
  const chunks = []; for await (const c of enc.readable) chunks.push(...c);
  p("TextEncoderStream", chunks); }

{ const dec = new TextDecoderStream();
  const wr = dec.writable.getWriter();
  (async () => { await wr.write(new TextEncoder().encode("héllo 🙂")); await wr.close(); })();
  let text = ""; for await (const c of dec.readable) text += c;
  p("TextDecoderStream", text); }

// --- a split multi-byte character across chunks ---
{ const bytes = new TextEncoder().encode("é🙂");
  const dec = new TextDecoderStream();
  const wr = dec.writable.getWriter();
  (async () => { await wr.write(bytes.slice(0, 3)); await wr.write(bytes.slice(3)); await wr.close(); })();
  let text = ""; for await (const c of dec.readable) text += c;
  p("split codepoint", text); }

console.log(L.join("\n"));

// --- a Blob's stream, and a response body, are real ReadableStreams ---
{ const b = new Blob(["blob text"]);
  const st = b.stream();
  p("Blob.stream is a stream", st instanceof ReadableStream);
  let text = ""; for await (const c of st.pipeThrough(new TextDecoderStream())) text += c;
  p("Blob.stream piped", text); }

{ const r = new Response("static body");
  p("Response.body is a stream", r.body instanceof ReadableStream);
  let text = ""; for await (const c of r.body.pipeThrough(new TextDecoderStream())) text += c;
  p("Response.body piped", text); }
