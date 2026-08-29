// Map/Set/array pipeline: the shape of request routing and data munging.
const t0=performance.now(); let acc=0;
for (let r=0;r<300;r++){
  const m=new Map(); const s=new Set();
  for(let i=0;i<1000;i++){ m.set("k"+i,i); s.add(i%97); }
  const vals=[...m.values()].filter(v=>v%3===0).map(v=>v*2);
  acc += vals.reduce((a,b)=>a+b,0) + s.size;
}
console.log("collections:", (performance.now()-t0).toFixed(1), "ms", acc);
