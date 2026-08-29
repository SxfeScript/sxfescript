const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const A=(n)=>Array.from({length:n},(_,i)=>i);
// basic removal / insertion / replacement
p("del mid",(()=>{const a=A(6);const r=a.splice(2,2);return[a,r]})());
p("del all",(()=>{const a=A(4);const r=a.splice(0);return[a,r]})());
p("del tail",(()=>{const a=A(5);const r=a.splice(3,99);return[a,r]})());
p("insert",(()=>{const a=A(4);const r=a.splice(2,0,"x","y");return[a,r]})());
p("replace fewer",(()=>{const a=A(6);const r=a.splice(1,3,"z");return[a,r]})());
p("replace more",(()=>{const a=A(4);const r=a.splice(1,1,"p","q","r");return[a,r]})());
p("negative start",(()=>{const a=A(5);const r=a.splice(-2,1);return[a,r]})());
p("start beyond",(()=>{const a=A(3);const r=a.splice(10,5,"t");return[a,r]})());
p("neg count",(()=>{const a=A(4);const r=a.splice(1,-1,"n");return[a,r]})());
p("no args",(()=>{const a=A(3);const r=a.splice();return[a,r]})());
// length and holes
p("length after",(()=>{const a=A(5);a.splice(1,2);return a.length})());
p("holes preserved",(()=>{const a=[0,,2,,4];const r=a.splice(1,2);return[a.length,0 in a,1 in a,r.length,0 in r]})());
p("trailing hole",(()=>{const a=[1,2,,];const r=a.splice(0,1);return[a.length,r,1 in a]})());
// non-array-ish and subclasses
p("array-like",(()=>{const o={0:"a",1:"b",2:"c",length:3};const r=Array.prototype.splice.call(o,1,1);return[o.length,o[0],o[1],r]})());
p("subclass",(()=>{class MyA extends Array{};const a=MyA.from([1,2,3]);const r=a.splice(1,1);return[a.length,r instanceof Array,[...a],[...r]]})());
// frozen / sealed must throw
p("frozen throws",(()=>{try{Object.freeze([1,2,3]).splice(0,1);return"no throw"}catch(e){return e.constructor.name}})());
p("sealed throws",(()=>{try{Object.seal([1,2,3]).splice(0,1);return"no throw"}catch(e){return e.constructor.name}})());
// element identity and refcounting
p("object elements",(()=>{const x={v:1},y={v:2};const a=[x,y,x];const r=a.splice(1,1);return[a.length,a[0]===x,a[1]===x,r[0]===y]})());
// repeated splices keep contents right
p("repeated",(()=>{const a=A(50);for(let i=0;i<20;i++){a.splice(10,1);a.splice(10,0,999)}return[a.length,a[10],a[9],a[11]]})());
// large array integrity
p("large integrity",(()=>{const a=A(2000);a.splice(1000,1);return[a.length,a[999],a[1000],a[1998],a[1999]]})());
// splice on array with accessors
p("accessor element",(()=>{try{const a=[1,2,3];Object.defineProperty(a,1,{get(){return"g"},configurable:true});const r=a.splice(1,1);return[r,a.length]}catch(e){return e.constructor.name}})());
p("accessor w/ setter",(()=>{const a=[1,2,3];let st=0;Object.defineProperty(a,1,{get(){return"g"},set(v){st=v},configurable:true});const r=a.splice(1,1);return[r,a.length,st]})());
p("proxy target",(()=>{const t=[1,2,3];const pr=new Proxy(t,{});const r=Array.prototype.splice.call(pr,1,1);return[r,t.length,[...t]]})());
console.log(L.join("\n"));
// A removing splice must not deoptimize the array: element reads afterwards
// should cost about what they did before. Ratio, not absolute time, so this
// holds on slow machines and in Debug builds.
function readNs(a){ const N=200000; let s=0; const t=performance.now();
  for(let i=0;i<N;i++) s+=a[i%a.length]; return ((performance.now()-t)/N)*1e6; }
const fresh = Array.from({length:800},(_,i)=>i);
readNs(fresh);
const before = readNs(fresh);
fresh.splice(400,1); fresh.push(0);
const after = readNs(fresh);
const ratio = after/before;
console.log(ratio < 2 ? "PASS" : "FAIL",
  "read before="+before.toFixed(1)+"ns after-splice="+after.toFixed(1)+"ns ratio="+ratio.toFixed(2));
if (ratio >= 2) throw new Error("splice deoptimized the array");
