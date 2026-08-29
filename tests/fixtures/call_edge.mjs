const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// native fn throwing must propagate with correct type
try{ null.x }catch(e){ p("npe", e.constructor.name) }
try{ JSON.parse("{") }catch(e){ p("json throw", e.constructor.name) }
try{ (1).toFixed(200) }catch(e){ p("range", e.constructor.name) }
// calling a non-function
try{ (void 0)() }catch(e){ p("not a fn", e.constructor.name) }
try{ ({}).nope() }catch(e){ p("method missing", e.constructor.name) }
// stack trace still produced
p("stack has frames", (()=>{try{null.x}catch(e){return e.stack.split("\n").length>1}})());
// deep recursion still throws RangeError, not a crash
// native fn as callback (Array.prototype.map with a C function)
p("native callback",[["1","2","3"].map(Number), [1.7,2.2].map(Math.floor)]);
// this-binding through native calls
p("this binding",[Array.prototype.join.call([1,2],"-"), Object.prototype.toString.call([])]);
// bound + apply + spread into natives
p("apply/spread",[Math.max.apply(null,[1,5,3]), Math.min(...[4,2,8]), Math.max.bind(null,10)(3)]);
// getters that call natives
p("getter native",(()=>{const o={get v(){return Math.abs(-4)}};return o.v})());
// Reflect.apply
p("reflect apply", Reflect.apply(Math.max,null,[2,9]));
// recursion through a native (sort comparator)
p("sort comparator",[3,1,2].sort((a,b)=>Math.sign(a-b)));
// tail call position
p("recursion depth 1000",(()=>{function t(n){ if(n===0) return "done"; return t(n-1) } return t(1000)})());
console.log(L.join("\n"));
