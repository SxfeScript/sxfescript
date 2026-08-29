const enc = new TextEncoder();
const out=[]; const chk=(n,g,w)=>out.push((JSON.stringify(g)===JSON.stringify(w)?"ok   ":"FAIL ")+n+" got="+JSON.stringify(g)+" want="+JSON.stringify(w));
function into(s, size) { const d = new Uint8Array(size); const r = enc.encodeInto(s, d); return {read:r.read, written:r.written, bytes:Array.from(d.subarray(0,r.written))}; }
chk("ascii fits",      into("abc", 10),    {read:3, written:3, bytes:[97,98,99]});
chk("exact fit",       into("abc", 3),     {read:3, written:3, bytes:[97,98,99]});
chk("too small",       into("abc", 2),     {read:2, written:2, bytes:[97,98]});
chk("empty dest",      into("abc", 0),     {read:0, written:0, bytes:[]});
chk("empty src",       into("", 4),        {read:0, written:0, bytes:[]});
chk("2-byte fits",     into("é", 4),       {read:1, written:2, bytes:[195,169]});
// must NOT split a 2-byte char when only 1 byte remains
chk("2-byte no split", into("aé", 2),      {read:1, written:1, bytes:[97]});
chk("3-byte no split", into("a日", 3),     {read:1, written:1, bytes:[97]});
chk("3-byte fits",     into("日", 3),      {read:1, written:3, bytes:[230,151,165]});
// surrogate pair = 4 bytes, read must count BOTH code units
chk("emoji fits",      into("🙂", 4),      {read:2, written:4, bytes:[240,159,153,130]});
chk("emoji no split",  into("🙂", 3),      {read:0, written:0, bytes:[]});
chk("emoji after a",   into("a🙂", 5),     {read:3, written:5, bytes:[97,240,159,153,130]});
// unpaired surrogate -> U+FFFD per WHATWG (matches Node and Bun)
chk("lone surrogate",  into("\uD800", 3),  {read:1, written:3, bytes:[239,191,189]});
const bad=out.filter(l=>l.startsWith("FAIL"));
console.log(bad.length?bad.join("\n"):"all "+out.length+" checks passed");
