import util from "node:util";
import { promisify, format, inherits, types } from "node:util";
import os from "node:os";
import qs from "node:querystring";
import { fileURLToPath, pathToFileURL } from "node:url";
import assert from "node:assert";
const L=[];const p=(n,v)=>L.push(n+"="+JSON.stringify(v));

// util.promisify over a node-style callback
const cbStyle = (a, b, cb) => cb(null, a + b);
p("promisify", await promisify(cbStyle)(2, 3));
p("promisify rejects", await promisify((cb) => cb(new Error("nope")))().catch(e => e.message));
p("promisify name", promisify(function named(cb){cb(null,1)}).name);

// util.format
p("format %s %d", format("%s has %d", "x", 3));
p("format %j", format("%j", {a:1}));
p("format %%", format("100%%"));
p("format extra args", format("a", 1, "b"));

// util.inherits
function Base(){} Base.prototype.hi = () => "hi";
function Derived(){} inherits(Derived, Base);
p("inherits", [new Derived().hi(), Derived.super_ === Base]);

// util.types
p("types", [types.isDate(new Date()), types.isRegExp(/x/), types.isMap(new Map()),
            types.isSet(new Set()), types.isPromise(Promise.resolve()),
            types.isTypedArray(new Uint8Array(1)), types.isDataView(new DataView(new ArrayBuffer(1)))]);
p("isDeepStrictEqual", [util.isDeepStrictEqual({a:[1]}, {a:[1]}), util.isDeepStrictEqual({a:1},{a:"1"})]);

// os
p("os platform is string", typeof os.platform() === "string");
p("os EOL", os.EOL);
p("os tmpdir non-empty", os.tmpdir().length > 0);

// querystring
p("qs parse", { ...qs.parse("a=1&b=two&a=3") });
p("qs stringify", qs.stringify({ a: 1, b: ["x","y"] }));
p("qs roundtrip", { ...qs.parse(qs.stringify({ k: "a b&c" })) });

// url
p("fileURLToPath", fileURLToPath("file:///tmp/x%20y.txt"));
p("pathToFileURL", String(pathToFileURL("/tmp/a b.txt")));
// fileURLToPath is native now: the scheme check, an empty localhost host,
// percent-decoding including a multi-byte character, and a URL object.
p("file url root", fileURLToPath("file:///"));
p("file url localhost", fileURLToPath("file://localhost/x/y"));
p("file url utf8", fileURLToPath("file:///a/%C3%A9.txt"));
p("file url plus", fileURLToPath("file:///a+b"));
p("file url object", fileURLToPath(new URL("file:///from/object")));
p("file url rejects http", (() => { try { fileURLToPath("http://x/y"); return "no"; }
  catch (e) { return e.constructor.name; } })());
p("round trip", fileURLToPath(pathToFileURL("/tmp/a b.txt")));

// assert
p("assert ok", (()=>{ assert(true); assert.ok(1); return "passed" })());
p("assert throws", (()=>{ try { assert(false, "boom"); return "no" } catch(e){ return e.name + ":" + e.message } })());
p("strictEqual", (()=>{ try { assert.strictEqual(1, 2); return "no" } catch(e){ return e.name } })());
p("deepStrictEqual pass", (()=>{ assert.deepStrictEqual({a:[1,{b:2}]}, {a:[1,{b:2}]}); return "passed" })());
p("deepStrictEqual fail", (()=>{ try { assert.deepStrictEqual({a:1},{a:2}); return "no" } catch(e){ return e.name } })());
p("assert.throws", (()=>{ assert.throws(()=>{ throw new Error("x") }); return "passed" })());
p("assert.match", (()=>{ assert.match("abc", /b/); return "passed" })());
console.log(L.join("\n"));
