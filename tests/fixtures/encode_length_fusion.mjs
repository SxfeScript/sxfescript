const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const enc = new TextEncoder();
for (const s of ["", "a", "héllo 🙂", "\ud800", "a\ud800b", "\udfff", "ñ".repeat(40), "\u{10FFFF}"])
  p("len " + JSON.stringify(s), enc.encode(s).length);
p("undefined", enc.encode(undefined).length);
p("absent", enc.encode().length);
// non-strings are coerced by encode; must match Node exactly
p("number", enc.encode(123).length);
p("null", enc.encode(null).length);
p("bool", enc.encode(true).length);
p("array", enc.encode([1,2]).length);
p("object", enc.encode({}).length);
p("toString obj", enc.encode({toString(){return "héllo"}}).length);
p("symbol", (()=>{try{return enc.encode(Symbol("s")).length}catch(e){return e.constructor.name}})());
// coercion side effect happens exactly once
let calls=0; enc.encode({toString(){calls++;return "xy"}});
p("coerced once", calls);
// the result must still be a real Uint8Array when it escapes
const u = enc.encode("héllo 🙂");
p("escapes", [u.length, u.byteLength, u[0], u instanceof Uint8Array, [...u.slice(0,3)]]);
p("buffer", u.buffer.byteLength);
// a fresh, independently mutable array each time
const x1 = enc.encode("abc"), x2 = enc.encode("abc");
x1[0] = 9;
p("independent", [x1[0], x2[0]]);
// encoding property untouched
p("encoding", enc.encoding);
// a second encoder instance works the same
p("second instance", new TextEncoder().encode("héllo 🙂").length);
// Guard invalidation: each must fall back to the ordinary call and read.

const call = () => enc.encode("héllo 🙂").length;
p("before", call());
{ const orig = TextEncoder.prototype.encode;
  TextEncoder.prototype.encode = function(){ return { length: 777 }; };
  p("encode replaced", call());
  TextEncoder.prototype.encode = orig; }
p("encode restored", call());
{ const TA = Object.getPrototypeOf(Uint8Array.prototype);
  const d = Object.getOwnPropertyDescriptor(TA, "length");
  Object.defineProperty(TA, "length", { get(){ return 555; }, configurable: true });
  p("TA length hooked", call());
  Object.defineProperty(TA, "length", d); }
p("TA restored", call());
{ Object.defineProperty(Uint8Array.prototype, "length", { get(){ return 444; }, configurable: true });
  p("u8 length hooked", call());
  delete Uint8Array.prototype.length; }
p("u8 restored", call());
// a per-instance own encode shadows the prototype
{ const e2 = new TextEncoder();
  e2.encode = function(){ return { length: 333 }; };
  p("own encode", e2.encode("héllo 🙂").length); }
console.log(L.join("\n"));
