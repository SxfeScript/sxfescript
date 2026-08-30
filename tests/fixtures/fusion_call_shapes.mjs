import { Buffer } from 'node:buffer';
const helper = () => "abc";
const o = { m: (x) => x, from: (a,b) => ({length: 99}) };
const enc = new TextEncoder();
const k = "m";
console.log(
  Buffer.from(helper(), "utf-8").length,
  enc.encode(helper()).length,
  Buffer.from(o.m("xy"), "utf-8").length,
  enc.encode(o[k]("xyz")).length,
  Buffer.from(o[k]("q") + helper(), "utf-8").length,
  o.from("a","b").length,
  "a,b,c".split(",").length,
  [1,2,3].filter(x=>x>1).length,
);
// A direct call in return position must keep its tail-call form, and a method
// call must keep its own; the recognizer sits in the same switch.
function helper2(){ return "abc"; }
function direct(){ return helper2(); }
function viaMethod(o){ return o.m(1); }
// `return f(...)` is the shape the transform applies to; a ternary is not,
// in this engine or in Node, so this stays a plain deep-but-bounded chain.
function chain(n){ if (n === 0) return "done"; return chain(n - 1); }
console.log(direct(), viaMethod({m:x=>x}), chain(5000));
