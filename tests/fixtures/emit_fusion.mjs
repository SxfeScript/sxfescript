import { EventEmitter } from 'node:events';
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
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
console.log(L.join("\n"));
