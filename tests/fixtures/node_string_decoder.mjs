// StringDecoder's whole job is not splitting a character across a chunk
// boundary. utf-8 is native now, so every way of cutting a multi-byte
// character in half has to come out the same as Node's.
import { StringDecoder } from "node:string_decoder";
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + n + " got=" + JSON.stringify(got) + (ok ? "" : " want=" + JSON.stringify(want))); };

const run = (chunks) => {
  const d = new StringDecoder("utf8");
  let out = "";
  for (const c of chunks) out += d.write(Buffer.from(c));
  return out + d.end();
};
check("ascii", run([[0x61, 0x62]]), "ab");
check("whole character", run([[0xe6, 0x97, 0xa5]]), "日");
check("split after one byte", run([[0xe6], [0x97, 0xa5]]), "日");
check("split after two", run([[0xe6, 0x97], [0xa5]]), "日");
check("byte at a time", run([[0xe6], [0x97], [0xa5]]), "日");
check("surrogate pair split", run([[0xf0, 0x9f], [0x8e, 0x89]]), "🎉");
check("two byte then ascii", run([[0xc3], [0xa9, 0x21]]), "é!");
check("stranded byte", run([[0xe6]]), "�");
check("stranded pair", run([[0xf0, 0x9f]]), "\ufffd");
check("nothing at all", run([[]]), "");
check("text through unchanged", new StringDecoder("utf8").write("already text"), "already text");
check("mixed chunks", run([[0x61], [0xe6, 0x97, 0xa5], [0x62]]), "a日b");
check("end with a chunk", (() => { const d = new StringDecoder("utf8");
  d.write(Buffer.from([0xe6, 0x97])); return d.end(Buffer.from([0xa5])); })(), "日");
console.log(bad === 0 ? "node:string_decoder: boundaries hold" : "FAILURES: " + bad);
if (bad !== 0) process.exit(1);
