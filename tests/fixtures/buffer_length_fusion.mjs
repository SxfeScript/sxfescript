import { Buffer } from 'node:buffer';
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// the fused shape, across content
for (const s of ["", "a", "payload 1", "héllo 🙂", "\ud800", "a\ud800b", "ñ".repeat(40)])
  p("len " + JSON.stringify(s), Buffer.from(s, "utf-8").length);
p("utf8 alias", Buffer.from("héllo 🙂", "utf8").length);
p("computed enc", (()=>{ const e = "ut" + "f-8"; return Buffer.from("héllo 🙂", e).length })());
// non-utf8 encodings must NOT take the fast path
p("hex", Buffer.from("616263", "hex").length);
p("base64", Buffer.from("aGVsbG8=", "base64").length);
p("latin1", Buffer.from("héllo", "latin1").length);
// non-string first argument
p("array arg", Buffer.from([1,2,3]).length);
p("u8 arg", Buffer.from(new Uint8Array(4)).length);
p("buffer arg", Buffer.from(Buffer.from("abc")).length);
// a non-string encoding must not take the fast path, and its coercion
// side effect must still happen
const order=[];
const enc={toString(){order.push("enc");return "utf-8"}};
p("object encoding", [Buffer.from("xy", enc).length, order.join(",")]);
// a non-string first argument falls back to the real Buffer.from
p("object arg throws", (()=>{try{ return Buffer.from({toString(){return "xy"}}, "utf-8").length }catch(e){return e.constructor.name}})());
// the result used for more than .length must be unaffected
const b = Buffer.from("payload 7", "utf-8");
p("real buffer", [b.length, b[0], b.toString("utf-8"), b instanceof Buffer]);
p("escapes", (()=>{ const x = Buffer.from("abc","utf-8"); return [x.length, x.byteLength, [...x]] })());
// .length on a different method call is untouched
p("other call", "abcdef".slice(1,4).length);
p("array from", Array.from([1,2,3]).length);
// errors propagate
p("throwing arg", (()=>{try{ return Buffer.from({toString(){throw new RangeError("x")}}, "utf-8").length }catch(e){return e.constructor.name}})());
p("bad type", (()=>{try{ return Buffer.from(null, "utf-8").length }catch(e){return e.constructor.name}})());
console.log(L.join("\n"));
// Guard invalidation: each of these must make the fused site fall back to the
// ordinary call and property read.
const inv = [];
{ const before = Buffer.from("héllo 🙂", "utf-8").length;
  Object.defineProperty(Buffer.prototype, "length", { get(){ return 111; }, configurable: true });
  inv.push([before, Buffer.from("héllo 🙂", "utf-8").length]);
  delete Buffer.prototype.length; }
{ const before = Buffer.from("héllo 🙂", "utf-8").length;
  const TA = Object.getPrototypeOf(Uint8Array.prototype);
  const d = Object.getOwnPropertyDescriptor(TA, "length");
  Object.defineProperty(TA, "length", { get(){ return 333; }, configurable: true });
  inv.push([before, Buffer.from("héllo 🙂", "utf-8").length]);
  Object.defineProperty(TA, "length", d); }
{ const before = Buffer.from("héllo 🙂", "utf-8").length;
  const orig = Buffer.from;
  Buffer.from = function(){ return { length: 999 }; };
  inv.push([before, Buffer.from("héllo 🙂", "utf-8").length]);
  Buffer.from = orig; }
console.log("invalidation=" + JSON.stringify(inv));
console.log("restored=" + Buffer.from("héllo 🙂", "utf-8").length);
