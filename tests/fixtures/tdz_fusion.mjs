const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
p("let += before init",(()=>{try{ x += 1; let x = 0; return "no throw" }catch(e){return e.constructor.name}})());
p("let ++ before init",(()=>{try{ y++; let y = 0; return "no throw" }catch(e){return e.constructor.name}})());
p("let -- before init",(()=>{try{ z--; let z = 0; return "no throw" }catch(e){return e.constructor.name}})());
p("let read before init",(()=>{try{ return q; let q=1 }catch(e){return e.constructor.name}})());
p("const reassign",(()=>{try{ const c=1; c+=1; return "no throw" }catch(e){return e.constructor.name}})());
p("closure TDZ",(()=>{try{ const f=()=>w+1; let r=f(); let w=1; return r }catch(e){return e.constructor.name}})());
p("block TDZ",(()=>{try{ { v += 1; let v = 0; } }catch(e){return e.constructor.name}})());
p("loop per-iteration let",(()=>{const fs=[];for(let i=0;i<3;i++){fs.push(()=>i)}return fs.map(f=>f())})());
// correctness of the fused ops themselves
p("overflow promotes",(()=>{let x=2147483640;for(let i=0;i<10;i++)x+=1;return x})());
p("float accum",(()=>{let x=0;for(let i=0;i<3;i++)x+=0.5;return x})());
p("string concat accum",(()=>{let s="";for(let i=0;i<3;i++)s+="a";return s})());
p("inc overflow",(()=>{let x=2147483647;x++;return x})());
p("dec underflow",(()=>{let x=-2147483648;x--;return x})());
p("bigint accum",(()=>{let x=0n;for(let i=0;i<3;i++)x+=1n;return x.toString()})());
p("mixed accum",(()=>{let x=1;x+="a";return x})());
p("post vs pre",(()=>{let a=1,b=1;return [a++, ++b, a, b]})());
console.log(L.join("\n"));
