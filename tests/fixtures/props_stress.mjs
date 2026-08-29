const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// empty object then grow
const a={}; p("empty keys",Object.keys(a));
a.x=1; p("one",[a.x,Object.keys(a)]);
a.y=2; a.z=3; p("three",[Object.keys(a),a.x+a.y+a.z]);
delete a.y; p("after delete",[Object.keys(a),a.y]);
a.y=9; p("re-add",[Object.keys(a),a.y]);
// many properties (force several resizes)
const b={}; for(let i=0;i<200;i++) b["k"+i]=i;
p("200 props",[Object.keys(b).length,b.k0,b.k199,Object.keys(b)[0],Object.keys(b)[199]]);
for(let i=0;i<200;i+=2) delete b["k"+i];
p("after deleting half",[Object.keys(b).length,b.k1,b.k0]);
// property-less objects of every flavor
p("Object.create(null)",[Object.keys(Object.create(null)).length]);
p("new (class{})",[Object.keys(new (class{})).length]);
p("Object.freeze({})",[Object.isFrozen(Object.freeze({}))]);
p("seal empty",[Object.isSealed(Object.seal({}))]);
// defineProperty on a fresh object
const c={}; Object.defineProperty(c,"g",{get(){return 42},enumerable:true,configurable:true});
p("getter",[c.g,Object.keys(c)]);
Object.defineProperty(c,"nv",{value:7,writable:false,enumerable:false});
p("non-enum",[c.nv,Object.keys(c),Object.getOwnPropertyNames(c).sort()]);
// spread / assign / JSON on empty and grown
p("spread empty",[JSON.stringify({...{}})]);
p("assign",[JSON.stringify(Object.assign({},{q:1},{r:2}))]);
p("json empty",[JSON.stringify({}),JSON.stringify({a:{}})]);
// prototype chain on property-less objects
const proto={inherited:5}; const d=Object.create(proto);
p("inherited",[d.inherited,Object.keys(d),"inherited" in d,d.hasOwnProperty("inherited")]);
d.own=1; p("own+inherited",[d.own,d.inherited,Object.keys(d)]);
// classes and methods
class K{constructor(){this.v=1}m(){return this.v}get gg(){return 2}}
const k=new K(); p("class",[k.v,k.m(),k.gg,Object.keys(k)]);
// typed arrays / buffers keep working
const u=new Uint8Array(4); u[0]=255; p("u8",[u[0],u.length,Object.keys(u)]);
const ab=new ArrayBuffer(8); p("ab",[ab.byteLength,Object.keys(ab)]);
const dv=new DataView(ab); dv.setInt32(0,123); p("dv",[dv.getInt32(0)]);
// functions get length/name
function fn(x,y){} p("fn",[fn.length,fn.name]);
const arrow=(a,b,c)=>0; p("arrow",[arrow.length,arrow.name]);
p("cfunc",[Math.max.length,Math.max.name,parseInt.length]);
// Map/Set/Error/RegExp/Date construct fine
p("builtins",[new Map([[1,2]]).get(1),new Set([1,1,2]).size,new Error("e").message,/a/.test("a"),typeof new Date().getTime()]);
// sealed/frozen with props
const f=Object.freeze({a:1}); let threw=false; try{"use strict";f.a=2}catch(e){threw=true}
p("frozen write",[f.a]);
// getOwnPropertyDescriptors on empty
p("descriptors empty",[Object.keys(Object.getOwnPropertyDescriptors({})).length]);
// for-in over empty and grown
let acc=""; for(const kk in {}) acc+=kk; p("forin empty",[acc]);
acc=""; for(const kk in {m:1,n:2}) acc+=kk; p("forin two",[acc]);
console.log(L.join("\n"));
