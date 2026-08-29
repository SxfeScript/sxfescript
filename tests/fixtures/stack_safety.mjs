const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// plain recursion must throw RangeError, never crash
p("plain", (()=>{ function f(){return f()} try{f()}catch(e){return e.constructor.name} })());
// recursion through a native (Array.map) must also unwind cleanly
p("via native", (()=>{ function g(n){ return [n].map(x=>g(x+1))[0] } try{g(0)}catch(e){return e.constructor.name} })());
// recursion through JSON.stringify replacer
p("via json", (()=>{ let d=0; try{ JSON.stringify({a:1},function r(k,v){ d++; return d<100000? JSON.parse(JSON.stringify({b:v},r)) : v }) }catch(e){return e.constructor.name} })());
// deeply nested data structures still parse
p("deep json", (()=>{ let s="";for(let i=0;i<400;i++)s+="[";s+="1";for(let i=0;i<400;i++)s+="]";
  try{ return Array.isArray(JSON.parse(s)) }catch(e){ return e.constructor.name } })());
// deep proto chain
p("deep proto", (()=>{ let o={v:1}; for(let i=0;i<2000;i++) o=Object.create(o); return o.v })());
// recover and keep running after an overflow
p("recovers", (()=>{ function f(){return f()} try{f()}catch(e){} return [1,2,3].map(x=>x*2) })());
// mutual recursion
p("mutual", (()=>{ function a(n){return b(n+1)} function b(n){return a(n+1)} try{a(0)}catch(e){return e.constructor.name} })());
console.log(L.join("\n"));
