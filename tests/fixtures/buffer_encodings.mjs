// Every encoding name Buffer accepts, in both directions, plus the shapes
// Node's readers are lenient about: hex that stops at the first bad pair, and
// base64 that skips what it cannot use, ends at '=', and reads one byte per
// code unit -- which is why an emoji terminates a base64 string.
// Expected values are Node's own output for these inputs, captured verbatim.
const WANT = {"round:utf8": ["68656c6c6f2077c3b6726c6420f09f998220616263", "hello w\u00f6rld \ud83d\ude42 abc", 21], "round:utf-8": ["68656c6c6f2077c3b6726c6420f09f998220616263", "hello w\u00f6rld \ud83d\ude42 abc", 21], "round:UTF-8": ["68656c6c6f2077c3b6726c6420f09f998220616263", "hello w\u00f6rld \ud83d\ude42 abc", 21], "round:Utf8": ["68656c6c6f2077c3b6726c6420f09f998220616263", "hello w\u00f6rld \ud83d\ude42 abc", 21], "round:latin1": ["68656c6c6f2077f6726c64203d4220616263", "hello w\u00f6rld =B abc", 18], "round:binary": ["68656c6c6f2077f6726c64203d4220616263", "hello w\u00f6rld =B abc", 18], "round:ascii": ["68656c6c6f2077f6726c64203d4220616263", "hello wvrld =B abc", 18], "round:ucs2": ["680065006c006c006f0020007700f60072006c00640020003dd842de2000610062006300", "hello w\u00f6rld \ud83d\ude42 abc", 36], "round:ucs-2": ["680065006c006c006f0020007700f60072006c00640020003dd842de2000610062006300", "hello w\u00f6rld \ud83d\ude42 abc", 36], "round:utf16le": ["680065006c006c006f0020007700f60072006c00640020003dd842de2000610062006300", "hello w\u00f6rld \ud83d\ude42 abc", 36], "round:utf-16le": ["680065006c006c006f0020007700f60072006c00640020003dd842de2000610062006300", "hello w\u00f6rld \ud83d\ude42 abc", 36], "round:hex": ["", "", 9], "round:base64": ["85e965a30ae5", "hellowrl", 13], "round:base64url": ["85e965a30ae5", "hellowrl", 13], "base64 out": "aGVsbG8gd8O2cmxkIPCfmYIgYWJj", "base64url out": "aGVsbG8gd8O2cmxkIPCfmYIgYWJj", "unknown throws": "TypeError", "hex:\"68656c\"": "68656c", "hex:\"68656c6\"": "68656c", "hex:\"zz\"": "", "hex:\"68zz6c\"": "68", "hex:\"\"": "", "hex:\"6\"": "", "hex:\"68656C6C\"": "68656c6c", "b64:\"hello w\u00f6rld \ud83d\ude42 abc\"": "85e965a30ae5", "b64:\"hello world\"": "85e965a30a2b95", "b64:\"aGVsbG8\"": "68656c6c6f", "b64:\"aGVsbG8=\"": "68656c6c6f", "b64:\"aG Vs bG8\"": "68656c6c6f", "b64:\"aG\\nVsbG8\"": "68656c6c6f", "b64:\"a\"": "", "b64:\"ab\"": "69", "b64:\"abc\"": "69b7", "b64:\"abcd\"": "69b71d", "b64:\"abcde\"": "69b71d", "b64:\"ab*cd\"": "69b71d", "b64:\"ab=cd\"": "69", "b64:\"-_-_\"": "fbffbf", "b64:\"+/+/\"": "fbffbf", "b64:\"  aGVs  bG8  \"": "68656c6c6f", "b64:\"aGVsbG8###\"": "68656c6c6f", "b64:\"####\"": "", "b64:\"aGVsbG8~\"": "68656c6c6f"};
let bad = 0;
const check = (n, got) => {
  const g = JSON.stringify(got);
  const w = JSON.stringify(WANT[n]);
  if (g !== w) { bad++; console.log("FAIL " + n + " got=" + g + " want=" + w); }
};

const src = "hello w\u00f6rld \u{1f642} abc";
const round = (enc) => [Buffer.from(src, enc).toString("hex"),
                        Buffer.from(src, enc).toString(enc),
                        Buffer.byteLength(src, enc)];
for (const enc of ["utf8","utf-8","UTF-8","Utf8","latin1","binary","ascii",
                   "ucs2","ucs-2","utf16le","utf-16le","hex","base64","base64url"])
  check("round:" + enc, round(enc));
check("base64 out", Buffer.from(src).toString("base64"));
check("base64url out", Buffer.from(src).toString("base64url"));
check("unknown throws",
      (() => { try { Buffer.from(src, "nope"); return "no"; } catch (e) { return e.constructor.name; } })());
for (const i of ["68656c","68656c6","zz","68zz6c","","6","68656C6C"])
  check("hex:" + JSON.stringify(i), Buffer.from(i, "hex").toString("hex"));
for (const i of ["hello w\u00f6rld \u{1f642} abc","hello world","aGVsbG8","aGVsbG8=","aG Vs bG8","aG\nVsbG8",
                 "a","ab","abc","abcd","abcde","ab*cd","ab=cd","-_-_","+/+/","  aGVs  bG8  ",
                 "aGVsbG8###","####","aGVsbG8~"])
  check("b64:" + JSON.stringify(i), Buffer.from(i, "base64").toString("hex"));

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
