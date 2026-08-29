const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// expression VALUE of compound assignment must survive where it is used
let a=1; const r1=(a+=2); p("value used",[r1,a]);
let b="x"; const r2=(b+="y"); p("string value",[r2,b]);
// assignment as argument / in condition / chained
let c=0; p("as arg",[Math.max(c+=5, 3), c]);
let d=0; if((d+=1)===1) p("in cond",d);
let e=1, f=1; f = (e += 3); p("chained",[e,f]);
// captured variable forms (the fused shape)
let g=0; const inc=(v)=>{ g+=v; }; inc(2); inc(3); p("captured stmt",g);
let h=0; const incv=(v)=> h+=v; p("captured expr value",[incv(4), h]);
// TDZ through captured compound assignment
p("captured TDZ",(()=>{try{ const fn=()=>{ q+=1; }; fn(); let q=0; return "no" }catch(err){return err.constructor.name}})());
// const capture must still throw
p("const capture",(()=>{try{ const k=1; const fn=()=>{ k+=1; }; fn(); return "no" }catch(err){return err.constructor.name}})());
// checked local store where value IS used afterwards
p("let value flows",(()=>{ let x; const y=(x=7); return [x,y] })());
// getter/setter interplay untouched
let sv=0; const o={ set s(v){ sv=v; }, get s(){ return 99; } };
p("setter value",[o.s = 5, sv]);
console.log(L.join("\n"));
