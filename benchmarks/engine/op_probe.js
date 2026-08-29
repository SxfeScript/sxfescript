// Absolute per-op cost, to be compared against another runtime.
function b(name, setup, op, iters) {
  const ctx = setup(); op(ctx, 0); op(ctx, 1);
  let best = Infinity;
  for (let r = 0; r < 3; r++) {
    const t = performance.now();
    for (let i = 0; i < iters; i++) op(ctx, i);
    const ns = ((performance.now() - t) / iters) * 1e6;
    if (ns < best) best = ns;
  }
  console.log(name + "\t" + best.toFixed(1));
}
const N = 20000;
b("regexp.test",      ()=>/^[a-z]+\d{2,4}$/, (r)=>r.test("abcdef123"), N);
b("regexp.exec+group",()=>/(?<w>[a-z]+)(?<d>\d+)/, (r)=>r.exec("abc123").groups.d, N);
b("str.replace regex", ()=>"a-b-c-d-e-f", (s)=>s.replace(/-/g,"+"), N);
b("str.split",        ()=>"a,b,c,d,e,f,g,h", (s)=>s.split(",").length, N);
b("str.match all",    ()=>"a1b2c3d4e5", (s)=>s.match(/\d/g).length, N);
b("array.sort(100)",  ()=>{const a=[];for(let i=0;i<100;i++)a.push((i*7919)%100);return a}, (a)=>a.slice().sort((x,y)=>x-y)[0], 2000);
b("array.join(100)",  ()=>{const a=[];for(let i=0;i<100;i++)a.push(i);return a}, (a)=>a.join(",").length, 5000);
b("array.includes",   ()=>{const a=[];for(let i=0;i<100;i++)a.push(i);return a}, (a)=>a.includes(99), N);
b("array.splice mid", ()=>({a:Array.from({length:200},(_,i)=>i)}), (c)=>{c.a.splice(100,1);c.a.splice(100,0,1)}, N);
b("array.unshift/shift",()=>({a:Array.from({length:200},(_,i)=>i)}), (c)=>{c.a.unshift(0);c.a.shift()}, N);
b("Object.keys(50)",  ()=>{const o={};for(let i=0;i<50;i++)o["k"+i]=i;return o}, (o)=>Object.keys(o).length, N);
b("Object.entries(50)",()=>{const o={};for(let i=0;i<50;i++)o["k"+i]=i;return o}, (o)=>Object.entries(o).length, 5000);
b("spread object(20)",()=>{const o={};for(let i=0;i<20;i++)o["k"+i]=i;return o}, (o)=>Object.keys({...o}).length, 5000);
b("delete+readd",     ()=>({o:{a:1,b:2,c:3}}), (c)=>{delete c.o.b;c.o.b=2}, N);
b("closure create",   ()=>({n:1}), (c)=>{const f=(x)=>x+c.n;return f(1)}, N);
b("new Error+stack",  ()=>0, ()=>new Error("x").stack.length, 5000);
b("Promise.resolve",  ()=>0, ()=>{Promise.resolve(1);return 1}, N);
b("Symbol key read",  ()=>{const s=Symbol("k");const o={[s]:1};return {s,o}}, (c)=>c.o[c.s], N);
b("getter chain(5)",  ()=>{let o={v:1};for(let i=0;i<5;i++){const p=o;o=Object.create(p)}return o}, (o)=>o.v, N);
b("ta.set(64)",       ()=>({d:new Uint8Array(64),s:new Uint8Array(64)}), (c)=>{c.d.set(c.s);return 1}, N);
b("ta.subarray",      ()=>new Uint8Array(1024), (t)=>t.subarray(0,64).length, N);
b("Date.now",         ()=>0, ()=>Date.now(), N);
b("JSON.parse(1k)",   ()=>JSON.stringify(Array.from({length:100},(_,i)=>({i,n:"x"+i}))), (s)=>JSON.parse(s).length, 2000);
