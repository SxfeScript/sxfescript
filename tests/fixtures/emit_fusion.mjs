import { EventEmitter } from 'node:events';
// Expected values are Node's own output for this file, captured verbatim.
const WANT = {
"fused sum": "[10,true,20]",
"no listener": "false",
"int overflow": "[2147485000,true]",
"float acc": "1.5",
"float arg": "1.5",
"nan": "null",
"string arg": "\"05\"",
"no arg": "null",
"extra args": "1",
"two listeners": "[\"a1\",\"b1\"]",
"once": "1",
"listener this": "true",
"remove during emit": "[\"f1\",\"f2\",\"f1\"]",
"throws": "\"RangeError:boom\"",
"unhandled error": "\"TypeError\"",
"off/on": "2",
"direct mutation": "[1,1]",
"events replaced": "[1,1]",
"events reshaped": "2",
"slot deleted": "[false,1]",
"emit shadowed": "[1,1]",
"two emitters": "[2,10]",
"two events": "[1,10]",
"removeAll": "[false,0]",
"subclass": "7",
"computed name": "7",
"count": "[1,1]"
};
let bad = 0;
const p = (n, v) => {
  const got = JSON.stringify(v);
  if (got !== WANT[n]) { bad++; console.log("FAIL " + n + " got=" + got + " want=" + WANT[n]); }
};
// the fused shape
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;});
  for(let i=0;i<5;i++) e.emit("x",i); p("fused sum",[c, e.emit("x",10), c]); }
// return value with no listener
{ const e=new EventEmitter(); p("no listener", e.emit("x",1)); }
// numeric promotion: overflow to double, floats, NaN
{ const e=new EventEmitter(); let c=2147483000; e.on("x",v=>{c+=v;});
  e.emit("x",2000); p("int overflow",[c, Number.isInteger(c)]); }
{ const e=new EventEmitter(); let c=0.5; e.on("x",v=>{c+=v;}); e.emit("x",1); p("float acc",c); }
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.emit("x",1.5); p("float arg",c); }
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.emit("x",NaN); p("nan",c); }
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.emit("x","5"); p("string arg",c); }
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.emit("x"); p("no arg",c); }
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.emit("x",1,2,3); p("extra args",c); }
// multiple listeners: order and all invoked
{ const e=new EventEmitter(); const o=[]; e.on("x",v=>o.push("a"+v)); e.on("x",v=>o.push("b"+v));
  e.emit("x",1); p("two listeners",o); }
// once
{ const e=new EventEmitter(); let n=0; e.once("x",()=>n++); e.emit("x"); e.emit("x"); p("once",n); }
// listener this
{ const e=new EventEmitter(); let self=null; e.on("x",function(){self=this;}); e.emit("x");
  p("listener this", self===e); }
// removal during emit
{ const e=new EventEmitter(); const o=[]; const f2=()=>o.push("f2");
  const f1=()=>{o.push("f1"); e.off("x",f2);}; e.on("x",f1); e.on("x",f2);
  e.emit("x"); e.emit("x"); p("remove during emit",o); }
// throwing listener propagates
{ const e=new EventEmitter(); e.on("x",()=>{throw new RangeError("boom");});
  p("throws",(()=>{try{e.emit("x",1);return "no"}catch(err){return err.constructor.name+":"+err.message}})()); }
// unhandled error event
{ const e=new EventEmitter();
  p("unhandled error",(()=>{try{e.emit("error",new TypeError("bad"));return "no"}catch(err){return err.constructor.name}})()); }
// off then re-on
{ const e=new EventEmitter(); let c=0; const f=v=>{c+=v;}; e.on("x",f); e.emit("x",1);
  e.off("x",f); e.emit("x",1); e.on("x",f); e.emit("x",1); p("off/on",c); }
// direct _events mutation must be seen
{ const e=new EventEmitter(); let c=0,d=0; e.on("x",v=>{c+=v;});
  e.emit("x",1); e._events.x = v=>{d+=v;}; e.emit("x",1); p("direct mutation",[c,d]); }
// replacing _events wholesale
{ const e=new EventEmitter(); let c=0,d=0; e.on("x",v=>{c+=v;});
  e.emit("x",1); e._events = { x: v=>{d+=v;} }; e.emit("x",1); p("events replaced",[c,d]); }
// adding a key to _events reshapes it
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;});
  e.emit("x",1); e._events.other = ()=>{}; e.emit("x",1); p("events reshaped",c); }
// deleting the listener slot
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;});
  e.emit("x",1); delete e._events.x; p("slot deleted",[e.emit("x",1), c]); }
// an own emit shadows the prototype's
{ const e=new EventEmitter(); let c=0,n=0; e.on("x",v=>{c+=v;});
  e.emit("x",1); e.emit = function(){ n++; return true; }; e.emit("x",1);
  p("emit shadowed",[c,n]); }
// a second emitter of the same shape resolves to its own listener
{ const e1=new EventEmitter(), e2=new EventEmitter(); let a=0,b=0;
  e1.on("x",v=>{a+=v;}); e2.on("x",v=>{b+=v;});
  e1.emit("x",1); e2.emit("x",10); e1.emit("x",1); p("two emitters",[a,b]); }
// different event name on the same emitter
{ const e=new EventEmitter(); let a=0,b=0; e.on("x",v=>{a+=v;}); e.on("y",v=>{b+=v;});
  e.emit("x",1); e.emit("y",10); p("two events",[a,b]); }
// removeAllListeners
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); e.removeAllListeners("x");
  p("removeAll",[e.emit("x",5), c]); }
// a subclass
{ class Sub extends EventEmitter {} const e=new Sub(); let c=0; e.on("x",v=>{c+=v;});
  e.emit("x",7); p("subclass",c); }
// computed event name
{ const e=new EventEmitter(); let c=0; e.on("x",v=>{c+=v;}); const k="x"; e.emit(k,3);
  e.emit("x".concat(""),4); p("computed name",c); }
// listenerCount / listeners still right
{ const e=new EventEmitter(); const f=()=>{}; e.on("x",f); p("count",[e.listenerCount("x"), e.listeners("x").length]); }
console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
