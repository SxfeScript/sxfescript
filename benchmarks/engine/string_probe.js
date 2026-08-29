function b(l,fn,n){ fn();fn(); let best=Infinity;
  for(let r=0;r<3;r++){const t=performance.now();for(let i=0;i<n;i++)fn(i);
  const ns=((performance.now()-t)/n)*1e6; if(ns<best)best=ns;}
  console.log(l+"\t"+best.toFixed(1)); }
const N=100000;
const csv="a,b,c,d,e,f,g,h";
b("split(string sep)",()=>csv.split(",").length,N);
b("split(regex sep)",()=>csv.split(/,/).length,N);
b("split(char limit)",()=>csv.split(",",3).length,N);
b("split('')",()=>"abcdefgh".split("").length,N);
b("indexOf(str)",()=>csv.indexOf("g"),N);
b("replace(str,str)",()=>csv.replace("a","z"),N);
b("replaceAll(str)",()=>csv.replaceAll(",","-"),N);
b("replace(re,str)",()=>csv.replace(/,/g,"-"),N);
b("regex.test simple",(()=>{const r=/^[a-z]+$/;return()=>r.test("abcdef")})(),N);
b("regex.test anchored",(()=>{const r=/^a/;return()=>r.test("abcdef")})(),N);
b("new RegExp each",()=>new RegExp("^[a-z]+$").test("abcdef"),20000);
b("startsWith",()=>csv.startsWith("a"),N);
b("template concat",(()=>{const a="x",bb="y";return()=>`${a}-${bb}-${a}`})(),N);
b("str.concat +",(()=>{const a="x",bb="y";return()=>a+"-"+bb+"-"+a})(),N);
