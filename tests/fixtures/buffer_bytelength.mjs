import { Buffer } from 'node:buffer';
const t=(d,f)=>{try{console.log(d, JSON.stringify(f()))}catch(e){console.log(d,"THROW",e.constructor.name)}};
t("utf8 default",()=>Buffer.byteLength("héllo 🙂"));
t("utf8 explicit",()=>Buffer.byteLength("héllo 🙂","utf-8"));
t("latin1",()=>Buffer.byteLength("héllo 🙂","latin1"));
t("ascii",()=>Buffer.byteLength("héllo 🙂","ascii"));
t("ucs2",()=>Buffer.byteLength("héllo 🙂","ucs2"));
t("utf16le",()=>Buffer.byteLength("héllo 🙂","utf16le"));
t("hex odd",()=>Buffer.byteLength("abc","hex"));
t("base64",()=>Buffer.byteLength("aGVsbG8=","base64"));
t("base64url",()=>Buffer.byteLength("aGVsbG8","base64url"));
t("buffer arg",()=>Buffer.byteLength(Buffer.from("abc")));
t("u8 arg",()=>Buffer.byteLength(new Uint8Array(5)));
t("ab arg",()=>Buffer.byteLength(new ArrayBuffer(7)));
t("dv arg",()=>Buffer.byteLength(new DataView(new ArrayBuffer(9))));
t("number",()=>Buffer.byteLength(123));
t("null",()=>Buffer.byteLength(null));
t("unpaired sur",()=>Buffer.byteLength("\ud800"));
t("bad encoding",()=>Buffer.byteLength("abc","nope"));
t("empty",()=>Buffer.byteLength(""));

// Brand checks: only real binary data is accepted, and a user-defined
// byteLength getter must never be consulted or run.
t("plain object",()=>Buffer.byteLength({byteLength:4}));
t("throwing getter",()=>Buffer.byteLength({get byteLength(){throw new Error("x")}}));
t("array-like",()=>Buffer.byteLength({length:3}));
t("shared array buffer",()=>Buffer.byteLength(new SharedArrayBuffer(6)));
t("proxy over object",()=>Buffer.byteLength(new Proxy({byteLength:4},{})));
t("float64 view",()=>Buffer.byteLength(new Float64Array(3)));
t("subarray",()=>Buffer.byteLength(new Uint8Array(10).subarray(2,7)));
t("dataview offset",()=>Buffer.byteLength(new DataView(new ArrayBuffer(10),2,5)));
// Encoding names are case-insensitive.
for (const e of ["utf8","UTF8","Utf-8","hex","HEX","Hex","base64","BASE64",
                 "base64url","BASE64URL","latin1","Latin1","LATIN1","binary","BINARY",
                 "ascii","ASCII","ucs2","UCS2","ucs-2","utf16le","UTF16LE","uTf16Le","utf-16le"])
  t("enc " + e,()=>Buffer.byteLength("héllo wörld",e));
t("unknown encoding",()=>Buffer.byteLength("héllo","nope"));
t("matches encode",()=>{const e=new TextEncoder();let ok=true;
  for(const s of ["","a","é","🙂","\ud800","a\ud800b","héllo 🙂 x","ñ".repeat(40)])
    if(Buffer.byteLength(s)!==e.encode(s).length) ok=false; return ok;});
