// Per-op cost at two sizes. Ratio near 1 => sublinear. High ratio => suspect.
function probe(name, setup, op, small, large) {
  const run = (n) => { const ctx = setup(n); const ITER = 50000;
    const t = performance.now(); for (let i=0;i<ITER;i++) op(ctx, i);
    return ((performance.now()-t)/ITER)*1e6; };
  run(small); const a = run(small), b = run(large);
  const r = b/a;
  console.log(name.padEnd(26), a.toFixed(1).padStart(7), b.toFixed(1).padStart(8), (r).toFixed(2).padStart(6), r>3?"  <-- SUSPECT":"");
}
const S=64, L=8192;
probe("Set.has",        n=>{const s=new Set();for(let i=0;i<n;i++)s.add(i);return {s,n}}, (c,i)=>c.s.has(i%c.n), S,L);
probe("Map.get",        n=>{const m=new Map();for(let i=0;i<n;i++)m.set(i,i);return {m,n}}, (c,i)=>c.m.get(i%c.n), S,L);
probe("Map.get(string)",n=>{const m=new Map();for(let i=0;i<n;i++)m.set("k"+i,i);return {m,n}}, (c,i)=>c.m.get("k"+(i%c.n)), S,L);
probe("Set.has(object)",n=>{const o=[],s=new Set();for(let i=0;i<n;i++){const x={i};o.push(x);s.add(x)}return {s,o,n}}, (c,i)=>c.s.has(c.o[i%c.n]), S,L);
probe("obj[key] read",  n=>{const o={};for(let i=0;i<n;i++)o["k"+i]=i;return {o,n}}, (c,i)=>c.o["k"+(i%c.n)], S,L);
probe("array[i] read",  n=>{const a=[];for(let i=0;i<n;i++)a.push(i);return {a,n}}, (c,i)=>c.a[i%c.n], S,L);
probe("array.push/pop", n=>{const a=[];for(let i=0;i<n;i++)a.push(i);return {a,n}}, (c)=>{c.a.push(1);c.a.pop()}, S,L);
probe("array.indexOf(0)",n=>{const a=[];for(let i=0;i<n;i++)a.push(i);return {a,n}}, (c)=>c.a.indexOf(0), S,L);
probe("str.charCodeAt", n=>({s:"x".repeat(n),n}), (c,i)=>c.s.charCodeAt(i%c.n), S,L);
probe("str.slice(0,8)", n=>({s:"x".repeat(n),n}), (c)=>c.s.slice(0,8).length, S,L);
probe("str.indexOf(z)", n=>({s:"x".repeat(n)+"z",n}), (c)=>c.s.indexOf("z"), S,L);
probe("JSON.stringify", n=>{const o={};for(let i=0;i<n;i++)o["k"+i]=i;return {o}}, (c)=>JSON.stringify(c.o).length, 8, 512);
probe("WeakMap.get",    n=>{const o=[],w=new WeakMap();for(let i=0;i<n;i++){const x={i};o.push(x);w.set(x,i)}return {w,o,n}}, (c,i)=>c.w.get(c.o[i%c.n]), S,L);
