// Template/string processing: the shape of a static-site or codegen step.
const rows=[]; for(let i=0;i<500;i++) rows.push({id:i,name:"item "+i,tag:"t"+(i%7)});
const t0=performance.now(); let len=0;
for (let r=0;r<200;r++){
  let out="";
  for (const row of rows) out += `<li data-id="${row.id}" class="${row.tag}">${row.name.toUpperCase()}</li>\n`;
  len += out.length;
}
console.log("text:", (performance.now()-t0).toFixed(1), "ms", len);
