const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));
p("fromAsync type", typeof Array.fromAsync);
p("fromAsync sync iterable", await Array.fromAsync([1,2,3]));
p("fromAsync promises", await Array.fromAsync([Promise.resolve(1), Promise.resolve(2)]));
p("fromAsync mapFn", await Array.fromAsync([1,2], async x => x*2));
async function* gen(){ yield 1; yield 2; yield 3; }
p("fromAsync async gen", await Array.fromAsync(gen()));
p("fromAsync arraylike", await Array.fromAsync({length:2, 0:"a", 1:"b"}));
p("zip type", typeof Iterator.zip);
p("zipKeyed type", typeof Iterator.zipKeyed);
if (typeof Iterator.zip === "function")
  p("zip result", [...Iterator.zip([[1,2],[3,4]])]);
if (typeof Iterator.zipKeyed === "function")
  p("zipKeyed result", [...Iterator.zipKeyed({a:[1,2], b:[3,4]})]);
console.log(L.join("\n"));
