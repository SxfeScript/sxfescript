// direct eval sees and creates vars in the enclosing scope (sloppy mode)
function f(){ var a=1; eval("var b=2;"); return a+b; }
console.log("direct eval var:", f());
function g(){ let x=10; return eval("x+1"); }
console.log("eval reads let:", g());
function h(){ var y=1; eval("function inner(){ y=5; } inner();"); return y; }
console.log("eval fn mutates:", h());
// with statement resolution
function w(){ var o={p:42}; with(o){ return p; } }
console.log("with:", w());
function w2(){ var o={q:1}, q=9; with(o){ return q; } }
console.log("with shadow:", w2());
// eval var colliding with outer let must throw
try { (function(){ let z=1; eval("var z=2;"); })(); console.log("collide: no throw"); }
catch(e){ console.log("collide:", e.constructor.name); }
console.log("global eval:", eval("var gtest=7; gtest"));
