import { StringDecoder } from "node:string_decoder";
import { isatty } from "node:tty";
import { createRequire, isBuiltin } from "node:module";
import { setTimeout as delay } from "node:timers/promises";
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));

// StringDecoder must not split a multi-byte character across chunks.
const bytes = new TextEncoder().encode("é🙂");
const d = new StringDecoder("utf8");
p("split codepoint", d.write(bytes.slice(0,3)) + d.write(bytes.slice(3)) + d.end());
p("isatty", isatty(1));
p("isBuiltin", [isBuiltin("fs"), isBuiltin("node:http"), isBuiltin("nope")]);
p("createRequire", typeof createRequire(import.meta.url));
p("timers/promises", await delay(5, "waited"));

// The V8 stack API packages inspect frames through.
Error.prepareStackTrace = (e, sites) => sites;
const sites = new Error().stack;
Error.prepareStackTrace = undefined;
p("callsites", Array.isArray(sites) && sites.length > 0);
const s0 = sites[0];
p("callsite api", ["getFileName","getLineNumber","getColumnNumber","getFunctionName",
  "isEval","isNative","isToplevel","getMethodName","getTypeName","toString"]
  .every((m) => typeof s0[m] === "function"));
p("callsite values", [typeof s0.getFileName(), typeof s0.getLineNumber(), s0.isEval()]);

// process stdio and hrtime
p("stdout fd", [process.stdout.fd, process.stderr.fd, process.stdout.isTTY]);
p("hrtime shape", (() => { const t = process.hrtime(); return Array.isArray(t) && t.length === 2; })());
p("hrtime.bigint", typeof process.hrtime.bigint());
console.log(L.join("\n"));
