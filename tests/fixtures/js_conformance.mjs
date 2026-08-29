const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v,(k,x)=>typeof x==="bigint"?x.toString()+"n":x));
// --- language ---
p("destructuring",(()=>{const{a,b:[c,...d]}={a:1,b:[2,3,4]};return[a,c,d]})());
p("spread/rest",(()=>{const f=(...r)=>r.length;return[f(1,2,3),[...[1,2],...[3]],{...{x:1},y:2}]})());
p("optional chain",(()=>{const o={a:{b:null}};return[o?.a?.b?.c,o.z?.q,o.a?.["b"]]})());
p("nullish",[null??"d",0??"d",undefined??"d",""||"e"]);
p("template",`x${1+1}y`);
p("tagged template",(()=>{const t=(s,...v)=>s.raw.join("|")+v.join(",");return t`a${1}b${2}c`})());
p("generators",(()=>{function*g(){yield 1;yield 2;return 3}return[...g()]})());
p("iterators",(()=>{const o={*[Symbol.iterator](){yield 1;yield 2}};return[...o]})());
p("labels/continue",(()=>{let n=0;outer:for(let i=0;i<3;i++){for(let j=0;j<3;j++){if(j)continue outer;n++}}return n})());
p("try/finally",(()=>{let r=[];try{r.push(1);throw 1}catch(e){r.push(2)}finally{r.push(3)}return r})());
p("getters/setters",(()=>{const o={_v:0,get v(){return this._v},set v(x){this._v=x*2}};o.v=5;return o.v})());
p("computed keys",(()=>{const k="dyn";return{[k+"1"]:1}})());
p("symbols",(()=>{const s=Symbol("t");const o={[s]:1};return[typeof s,o[s],s.description]})());
p("classes",(()=>{class A{static s=1;#p=2;constructor(){this.q=3}get gp(){return this.#p}static m(){return"sm"}}
  class B extends A{constructor(){super();this.r=4}}
  const b=new B();return[A.s,b.gp,b.q,b.r,A.m(),b instanceof A]})());
p("class static block",(()=>{class C{static v;static{C.v=42}}return C.v})());
// --- builtins ---
p("array methods",[[3,1,2].sort(),[1,2,3].map(x=>x*2),[1,2,3].filter(x=>x>1),[1,2,3].reduce((a,b)=>a+b),
  [1,[2,[3]]].flat(2),[1,2].flatMap(x=>[x,x]),[1,2,3].at(-1),[1,2,3].findLast(x=>x<3),[3,1,2].toSorted()]);
p("string methods",["abc".at(-1),"a-b".replaceAll("-","+"),"  x ".trim(),"ab".padStart(4,"0"),
  "abc".includes("b"),[..."aé\u{1F600}"].length,"ABC".toLowerCase(),"a,b".split(",")]);
p("object statics",[Object.entries({a:1}),Object.fromEntries([["b",2]]),Object.assign({},{c:3}),
  Object.getOwnPropertyNames({d:4}),Object.hasOwn({e:5},"e")]);
p("number/math",[Number.isInteger(5),Number.parseFloat("1.5"),(255).toString(16),Math.trunc(-1.7),
  0.1+0.2,Number.MAX_SAFE_INTEGER,parseInt("08")]);
p("bigint",[(2n**64n).toString(),typeof 1n,(BigInt(5)*2n).toString()]);
p("json",[JSON.stringify({a:[1,{b:2}]}),JSON.parse('{"x":[1,2]}').x[1],JSON.stringify({u:undefined,f(){}})]);
p("map/set",(()=>{const m=new Map([[1,"a"]]);const s=new Set([1,1,2]);
  return[m.get(1),m.size,[...s],s.has(2),[...m.entries()]]})());
p("weak",(()=>{const k={};const w=new WeakMap([[k,1]]);return[w.get(k),w.has({})]})());
p("regexp",["a1b2".replace(/\d/g,"#"),/(?<y>\d{4})/.exec("2024").groups.y,"aAb".match(/a/gi).length,
  /a/y.test("a"),"x".replace(/(x)/,"[$1]")]);
p("date",(()=>{const d=new Date(0);return[d.getTime(),d.toISOString()]})());
p("error types",[TypeError.name,(new RangeError("m")).message,(()=>{try{null.x}catch(e){return e.constructor.name}})()]);
p("proxy/reflect",(()=>{const pr=new Proxy({},{get:(t,k)=>k==="z"?9:Reflect.get(t,k)});return[pr.z,Reflect.has({a:1},"a")]})());
p("typed arrays",(()=>{const t=new Int16Array([1,-2]);const dv=new DataView(new ArrayBuffer(4));dv.setFloat32(0,1.5);
  return[t[1],t.length,dv.getFloat32(0),new Uint8Array([1,2]).toHex?.()??"n/a"]})());
p("symbol wellknown",(()=>{class X{static [Symbol.hasInstance](v){return v===1}}return[1 instanceof X]})());
p("getter on prototype chain",(()=>{const b={get g(){return"b"}};const d=Object.create(b);return d.g})());
p("closures/hoisting",(()=>{const fs=[];for(let i=0;i<3;i++)fs.push(()=>i);return fs.map(f=>f())})());
p("async shape",typeof (async()=>{})().then);
console.log(L.join("\n"));
