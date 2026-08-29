const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// reassigning a plain function's prototype must be seen immediately
function F(){}; const P1={tag:"first"}; F.prototype=P1;
let a=[]; for(let i=0;i<4;i++) a.push(Object.getPrototypeOf(new F()).tag);
const P2={tag:"second"}; F.prototype=P2;
for(let i=0;i<4;i++) a.push(Object.getPrototypeOf(new F()).tag);
p("reassigned prototype",a);
// interleaving two constructors (shared shape) must not cross
function G(){}; G.prototype={tag:"G"};
function H(){}; H.prototype={tag:"H"};
let b=[]; for(let i=0;i<6;i++) b.push(Object.getPrototypeOf(new (i%2?G:H)()).tag);
p("interleaved ctors",b);
// classes
class C{}; class D extends C{};
let c=[]; for(let i=0;i<4;i++){const o=new D(); c.push(o instanceof D, o instanceof C)}
p("class chain",[c[0],c[1],c[6],c[7]]);
// subclassed builtins
class MyArr extends Uint8Array{}; class MyMap extends Map{};
let d=[]; for(let i=0;i<4;i++){const o=new MyArr(3); d.push(o.length,o instanceof MyArr,o instanceof Uint8Array)}
p("subclass typed array",[d[0],d[1],d[2]]);
p("subclass Map",[new MyMap([[1,2]]).get(1), new MyMap() instanceof Map]);
// Reflect.construct with explicit newTarget
function Base(){}; function Other(){}; Other.prototype={tag:"other"};
let e=[]; for(let i=0;i<4;i++) e.push(Object.getPrototypeOf(Reflect.construct(Base,[],Other)).tag);
p("Reflect.construct newTarget",e);
// prototype reached through a Proxy get trap (never cacheable as a data slot)
let n=0;
const trapped=new Proxy(function(){}, {get(t,k,r){ if(k==="prototype"){n++;return {tag:"acc"+n};} return Reflect.get(t,k,r);}});
function Base3(){}
let f=[]; for(let i=0;i<3;i++) f.push(Object.getPrototypeOf(Reflect.construct(Base3,[],trapped)).tag);
p("proxy prototype trap",[f,n>=3]);
// a bound function has no own prototype: lookup must not invent one
function Src(){}; Src.prototype={tag:"src"};
const bnd=Src.bind(null);
let f2=[]; for(let i=0;i<3;i++) f2.push(Object.getPrototypeOf(new bnd()).tag);
p("bound function",f2);
// non-object prototype falls back to the realm's default
function B2(){}; B2.prototype=42;
p("non-object prototype",[Object.getPrototypeOf(new B2())===Object.prototype]);
// deleting prototype
function E2(){}; const saved=E2.prototype;
let g=[]; for(let i=0;i<3;i++) g.push(Object.getPrototypeOf(new E2())===saved);
p("stable prototype",g);
// typed arrays through subclass with buffers
const mv=new MyArr(4); mv[0]=7; p("subclass write",[mv[0],mv.buffer.byteLength]);
console.log(L.join("\n"));
