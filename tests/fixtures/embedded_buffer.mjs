const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const enc=new TextEncoder();
const u=enc.encode("hello wörld");
p("basic",[u.length,u[0],u[u.length-1]]);
const ab=u.buffer;
p("buffer",[ab.byteLength,ab.byteLength===u.length]);
p("f64 view aligned",(()=>{const b=enc.encode("0123456789abcdef").buffer;const dv=new DataView(b);dv.setFloat64(0,1.5);return dv.getFloat64(0)})());
p("slice",[...new Uint8Array(ab.slice(0,5))]);
p("transfer/detach",(()=>{ if(!ab.transfer) return "n/a"; const t=ab.transfer(); return [t.byteLength, ab.byteLength]; })());
const b2=Buffer.from("hi","utf-8");
p("buffer path",[b2.length,b2.toString("hex"),b2.buffer.byteLength===b2.length]); // exact-size buffer (Node pools into 8KB slabs here)
const big=enc.encode("x".repeat(100000));
p("large",[big.length,big[99999]]);
let sum=0; for(let i=0;i<200000;i++){ sum+=enc.encode("tick").length; }
p("churn",sum);
console.log(L.join("\n"));
