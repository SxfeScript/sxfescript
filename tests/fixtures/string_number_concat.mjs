const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
// basic
p("basic","x"+1);
p("zero","x"+0);
p("neg","x"+-5);
p("min int","x"+(-2147483648));
p("max int","x"+2147483647);
// -0 must print as "0" for ints
p("neg zero","x"+(-0));
p("neg zero float","x"+(-0.0));
// float vs int boundary
p("float","x"+1.5);
p("int-valued float","x"+2.0);
p("1e21","x"+1e21);
p("NaN","x"+NaN);
p("Inf","x"+Infinity);
p("-Inf","x"+(-Infinity));
// wide (utf-16) left operand
p("wide","é"+42);
p("emoji","🙂"+7);
p("wide neg","é"+(-3));
// empty left
p("empty",""+123);
// long left operand, forces realloc path
p("long",("a".repeat(100)+9).length);
// repeated append in a loop (exercises the in-place path)
let s=""; for(let i=0;i<10;i++) s+=i;
p("loop append",s);
let w="é"; for(let i=0;i<5;i++) w+=i;
p("loop wide",w);
// shared string must not be mutated in place
const base="shared";
const a=base+1, b=base+2;
p("no aliasing",[base,a,b]);
// aliasing through a second reference
let q="dup"; const q2=q; q=q+7;
p("alias 2",[q,q2]);
// int on the left is NOT this path
p("int left",1+"x");
p("int+int",1+2);
// bigint untouched
p("bigint","x"+10n);
// string concat still fine
p("str+str","a"+"b");
// += form
let acc="n"; acc+=5; acc+=6;
p("plus equals",acc);
// template still right
p("template",`${"v"}${8}`);
// length and charCodeAt after wide append
const ww="é"+12; p("wide detail",[ww.length,ww.charCodeAt(0),ww.charCodeAt(1),ww.charCodeAt(2)]);
console.log(L.join("\n"));
