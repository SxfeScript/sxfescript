const N=1000000; const keep=new Array(512);
function run(label,fn){ for(let i=0;i<50000;i++)fn(i); let best=Infinity;
  for(let r=0;r<3;r++){const t=performance.now();
    for(let i=0;i<N;i++) keep[i&511]=fn(i);
    const ns=((performance.now()-t)/N)*1e6; if(ns<best)best=ns;}
  console.log(label.padEnd(34), best.toFixed(1), "ns"); }
run("{a,b} literal (cold)", (i)=>({a:i,b:i+1}));
// pin an object whose shape is the {a} intermediate, and the {a,b} final
const pinA = {a:0};
const pinAB = {a:0,b:0};
run("{a,b} literal (pinned {a})", (i)=>({a:i,b:i+1}));
console.log("pins:", pinA.a, pinAB.b);
