// Standard JS: integer overflow must promote to double, never wrap.
function accum() { let x = 2147483640; for (let i = 0; i < 10; i++) x += 1; return x; }
function big()   { let x = 0; for (let i = 0; i < 5; i++) x += 1000000000; return x; }
console.log("accum:", accum(), "expect 2147483650");
console.log("big:  ", big(),   "expect 5000000000");
console.log("float:", (()=>{ let x=0; for(let i=0;i<3;i++) x += 0.5; return x; })(), "expect 1.5");
