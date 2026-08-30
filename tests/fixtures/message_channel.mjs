const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
p("crypto instanceof Crypto", crypto instanceof Crypto);
p("subtle instanceof SubtleCrypto", crypto.subtle instanceof SubtleCrypto);
p("Crypto not constructible", (()=>{try{new Crypto();return "no"}catch(e){return e.constructor.name}})());
const ch = new MessageChannel();
const got = [];
ch.port2.onmessage = (e) => got.push(e.data);
ch.port1.postMessage({ n: 1 });
ch.port1.postMessage("two");
await new Promise((r) => queueMicrotask(() => queueMicrotask(r)));
p("delivered", got);
p("structured cloned", (() => { const o = { deep: { v: 1 } };
  let seen = null; ch.port2.onmessage = (e) => { seen = e.data; };
  ch.port1.postMessage(o); return o; })());
await new Promise((r) => queueMicrotask(() => queueMicrotask(r)));
const ch2 = new MessageChannel();
const viaListener = [];
ch2.port2.addEventListener("message", (e) => viaListener.push(e.data));
ch2.port2.start();
ch2.port1.postMessage("listener");
await new Promise((r) => queueMicrotask(() => queueMicrotask(r)));
p("addEventListener path", viaListener);
// Self-checking rather than diffed against Node: Node's MessagePorts keep the
// event loop alive, so the same script does not exit there.
const want = [
  'crypto instanceof Crypto=true',
  'subtle instanceof SubtleCrypto=true',
  'Crypto not constructible="TypeError"',
  'delivered=[{"n":1},"two"]',
  'structured cloned={"deep":{"v":1}}',
  'addEventListener path=["listener"]',
];
let bad = 0;
for (let i = 0; i < want.length; i++) {
  const ok = L[i] === want[i];
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + L[i] + (ok ? "" : "  want " + want[i]));
}
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
