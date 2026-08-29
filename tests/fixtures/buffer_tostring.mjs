import { Buffer } from "node:buffer";
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const b = Buffer.from("hello wörld 🙂", "utf-8");
p("default", b.toString());
p("utf-8", b.toString("utf-8"));
p("utf8", b.toString("utf8"));
p("hex", b.toString("hex"));
p("base64", b.toString("base64"));
p("base64url", Buffer.from("??>>", "utf-8").toString("base64url"));
p("UTF-8 upper", b.toString("UTF-8"));
p("HEX upper", b.toString("HEX"));
p("Base64 mixed", b.toString("Base64"));
const l = Buffer.from([0xff, 0x41, 0x00]);
p("latin1", l.toString("latin1"));
p("binary", l.toString("binary"));
p("ascii", l.toString("ascii"));
p("empty", Buffer.alloc(0).toString("hex"));
try { b.toString("nope"); p("unknown","no throw"); } catch(e) { p("unknown", e.constructor.name); }
try { b.toString(123); p("number enc","no throw"); } catch(e) { p("number enc", e.constructor.name); }
try { b.toString(null); p("null enc","no throw"); } catch(e) { p("null enc", e.constructor.name); }
p("roundtrip", Buffer.from(b.toString("hex"), "hex").toString("utf-8"));
p("subarray", b.subarray(0,5).toString("utf-8"));
console.log(L.join("\n"));
