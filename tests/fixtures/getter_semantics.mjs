const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// getter called every time, receiver correct, side effects preserved
let calls=0; const o={_v:0,get g(){calls++;return ++this._v}};
let r=[]; for(let i=0;i<5;i++) r.push(o.g);
p("side effects",[r,calls,o._v]);
// getter on the prototype, many instances (receiver must differ)
class C{constructor(v){this._v=v} get val(){return this._v*2}}
const xs=[new C(1),new C(2),new C(3)]; let s=[];
for(let i=0;i<9;i++) s.push(xs[i%3].val);
p("proto getter receivers", s);
// redefining the getter mid-loop must be seen
const d={get k(){return "first"}};
let a1=[]; for(let i=0;i<3;i++) a1.push(d.k);
Object.defineProperty(d,"k",{get(){return "second"},configurable:true});
for(let i=0;i<3;i++) a1.push(d.k);
p("redefined getter",a1);
// replacing getter with a data property
Object.defineProperty(d,"k",{value:"data",configurable:true,writable:true});
let a2=[]; for(let i=0;i<3;i++) a2.push(d.k);
p("getter -> data",a2);
// data -> getter
const e={m:"data"}; let a3=[]; for(let i=0;i<3;i++) a3.push(e.m);
Object.defineProperty(e,"m",{get(){return "getter"},configurable:true});
for(let i=0;i<3;i++) a3.push(e.m);
p("data -> getter",a3);
// getter that throws
const t={get boom(){throw new TypeError("nope")}};
let caught=0; for(let i=0;i<3;i++){try{t.boom}catch(err){if(err instanceof TypeError)caught++}}
p("throwing getter",caught);
// getter-only (no setter) returns undefined when getter removed
const u={}; Object.defineProperty(u,"z",{set(v){},configurable:true});
let a4=[]; for(let i=0;i<3;i++) a4.push(u.z);
p("setter-only reads undefined",a4);
// typed array .length across shapes and after detach-like resize
const t1=new Uint8Array(4), t2=new Uint8Array(9), b1=Buffer.from("abc");
let a5=[]; for(let i=0;i<6;i++) a5.push(t1.length,t2.length,b1.length);
p("ta lengths",[a5[0],a5[1],a5[2],a5[15],a5[16],a5[17]]);
// shadowing .length with an own property
const t3=new Uint8Array(4); Object.defineProperty(t3,"length",{value:99,configurable:true});
let a6=[]; for(let i=0;i<3;i++) a6.push(t3.length);
p("shadowed length",a6);
// getter inherited through a deep chain, then shadowed mid-chain
const base={get w(){return "base"}}; const mid=Object.create(base); const leaf=Object.create(mid);
let a7=[]; for(let i=0;i<3;i++) a7.push(leaf.w);
Object.defineProperty(mid,"w",{get(){return "mid"},configurable:true});
for(let i=0;i<3;i++) a7.push(leaf.w);
p("deep shadow",a7);
// getter on a frozen object
const f=Object.freeze({get q(){return 7}});
let a8=[]; for(let i=0;i<3;i++) a8.push(f.q);
p("frozen getter",a8);
// accessor via class static + super
class P2{static get sv(){return "p"}} class Q2 extends P2{}
let a9=[]; for(let i=0;i<3;i++) a9.push(Q2.sv);
p("static inherited",a9);
console.log(L.join("\n"));
