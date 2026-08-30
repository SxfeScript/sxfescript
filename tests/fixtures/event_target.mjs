const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const t = new EventTarget();
let seen = [];
const a = (e) => seen.push("a:" + e.type);
t.addEventListener("x", a);
t.dispatchEvent(new Event("x"));
p("basic", seen);
// duplicate registration is ignored
t.addEventListener("x", a);
seen = []; t.dispatchEvent(new Event("x")); p("no duplicate", seen);
// removal
t.removeEventListener("x", a);
seen = []; t.dispatchEvent(new Event("x")); p("removed", seen);
// once
let n = 0; t.addEventListener("y", () => n++, { once: true });
t.dispatchEvent(new Event("y")); t.dispatchEvent(new Event("y")); p("once", n);
// order, and mutation during dispatch
const order = [];
const t2 = new EventTarget();
t2.addEventListener("z", () => { order.push(1); t2.addEventListener("z", () => order.push(3)); });
t2.addEventListener("z", () => order.push(2));
t2.dispatchEvent(new Event("z")); p("order + mutation", order);
// stopImmediatePropagation
const t3 = new EventTarget(); const got = [];
t3.addEventListener("s", (e) => { got.push("first"); e.stopImmediatePropagation(); });
t3.addEventListener("s", () => got.push("second"));
t3.dispatchEvent(new Event("s")); p("stopImmediate", got);
// cancelable and return value
const t4 = new EventTarget();
t4.addEventListener("c", (e) => e.preventDefault());
p("cancelable true", t4.dispatchEvent(new Event("c", { cancelable: true })));
p("cancelable false", t4.dispatchEvent(new Event("c", { cancelable: false })));
// handleEvent object
const t5 = new EventTarget(); let ho = 0;
t5.addEventListener("h", { handleEvent() { ho++; } });
t5.dispatchEvent(new Event("h")); p("handleEvent", ho);
// target and currentTarget
const t6 = new EventTarget(); let tgt = null;
t6.addEventListener("t", function (e) { tgt = e.target === t6 && e.currentTarget === t6; });
t6.dispatchEvent(new Event("t")); p("target set", tgt);
// CustomEvent detail
const t7 = new EventTarget(); let d = null;
t7.addEventListener("d", (e) => { d = e.detail; });
t7.dispatchEvent(new CustomEvent("d", { detail: { k: 1 } })); p("custom detail", d);
// AbortSignal removes the listener
const t8 = new EventTarget(); const ac = new AbortController(); let ab = 0;
t8.addEventListener("q", () => ab++, { signal: ac.signal });
t8.dispatchEvent(new Event("q")); ac.abort();
t8.dispatchEvent(new Event("q")); p("signal removes", ab);
// subclassing, which is how packages use it
class Bus extends EventTarget { ping() { this.dispatchEvent(new Event("ping")); } }
const b = new Bus(); let pings = 0;
b.addEventListener("ping", () => pings++); b.ping(); b.ping();
p("subclass", pings);
p("dispatch non-event", (()=>{try{ t.dispatchEvent({}); return "no" }catch(e){ return e.constructor.name }})());
console.log(L.join("\n"));
