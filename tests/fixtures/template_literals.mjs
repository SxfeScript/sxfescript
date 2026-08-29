const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
const x=1,y="b";
p("basic",`a${x}b${y}c`);
p("empty",`${""}`);
p("no subst",`plain`);
p("only subst",`${x}`);
p("adjacent",`${x}${y}`);
p("leading",`${x}tail`);
p("trailing",`head${x}`);
p("nested",`o${`i${x}`}o`);
p("newline",`a\nb${x}`);
p("escapes",`\t\\${x}A`);
// ToString vs ToPrimitive: template must use toString, not valueOf
const o={valueOf(){return 1},toString(){return "S"}};
p("ToString not valueOf",`${o}`);
p("plus uses valueOf",""+o);
// order of evaluation, left to right
const ord=[];const t=(n)=>({toString(){ord.push(n);return n}});
p("eval order",[`${t("a")}${t("b")}${t("c")}`,ord.join("")]);
// throwing toString propagates and leaves no residue
p("throws",(()=>{try{return `${{toString(){throw new TypeError("boom")}}}`}catch(e){return e.constructor.name+":"+e.message}})());
// Symbol must throw TypeError
p("symbol",(()=>{try{return `${Symbol("s")}`}catch(e){return e.constructor.name}})());
// null/undefined/number/bool/bigint/array/object
p("values",`${null}|${undefined}|${1.5}|${true}|${10n}|${[1,2]}|${{}}`);
// -0 and NaN and Infinity
p("edge nums",`${-0}|${NaN}|${Infinity}|${-Infinity}`);
// large template
p("many",`${1}${2}${3}${4}${5}${6}${7}${8}${9}${10}`);
// wide chars and surrogates
p("unicode",`é${"🙂"}${"\u{1F600}"}`);
// tagged templates must be unaffected
const tag=(s,...v)=>s.raw.join("|")+"#"+v.join(",");
p("tagged",tag`a${x}b${y}c`);
// String.prototype.concat itself still works
p("concat method","a".concat(1,"b"));
// template in a loop, and length
let acc=""; for(let i=0;i<3;i++) acc+=`${i}-`;
p("loop",acc);
p("length",`${"ab"}${"cd"}`.length);
console.log(L.join("\n"));
