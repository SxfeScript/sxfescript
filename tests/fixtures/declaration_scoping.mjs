const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const ev=(src)=>{ try{ (0,eval)(src); return "ok" }catch(e){ return e.constructor.name } };
// redeclaration errors must still fire
p("let+let",       ev("let a1=1; let a1=2;"));
p("let+var",       ev("let a2=1; var a2=2;"));
p("var+let",       ev("var a3=1; let a3=2;"));
p("const+const",   ev("const a4=1; const a4=2;"));
p("let+const",     ev("let a5=1; const a5=2;"));
p("var+var ok",    ev("var a6=1; var a6=2;"));
p("fn+let",        ev("function a7(){}; let a7=1;"));
p("class+let",     ev("class a8{}; let a8=1;"));
p("param+let",     ev("(function(x){ let x=1; })"));
p("catch+let",     ev("try{}catch(e){ let e=1; }"));
// block scoping still isolates
p("block shadow",  ev("let b1=1; { let b1=2; } "));
p("nested blocks", ev("{ let c1=1; { let c1=2; { let c1=3; } } }"));
p("for-let",       ev("for(let i=0;i<1;i++){ let i2=i; }"));
p("fn scope",      ev("function f(){ let d1=1; } function g(){ let d1=2; }"));
// values and TDZ still right
p("shadow values", (()=>{ let x=1; { let x=2; } return x })());
p("closure capture",(()=>{ const fs=[]; for(let i=0;i<3;i++) fs.push(()=>i); return fs.map(f=>f()) })());
p("tdz still",     (()=>{ try{ y; let y=1 }catch(e){ return e.constructor.name } })());
p("hoisted var",   (()=>{ return typeof z })() + "|" + (()=>{ var z=1; return typeof z })());
// many declarations still all reachable and distinct
p("many decls",(()=>{ const src="let "+Array.from({length:300},(_,i)=>"q"+i+"="+i).join(",")+"; q0+q299"; return (0,eval)(src) })());
p("many var",  (()=>{ const src="var "+Array.from({length:300},(_,i)=>"r"+i+"="+i).join(",")+"; r0+r299"; return (0,eval)(src) })());
p("dup in many",(()=>{ const src="let "+Array.from({length:300},(_,i)=>"s"+i+"="+i).join(",")+"; let s150=9;"; return ev(src) })());
console.log(L.join("\n"));
