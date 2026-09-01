// The builtins added on top of the original 24: what each one actually does,
// not just that it resolves.
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
let bad = 0;
const check = (name, ok, detail) => {
  if (!ok) bad++;
  console.log((ok ? "ok   " : "FAIL ") + name + (detail === undefined ? "" : " " + detail));
};

const all = require("module").builtinModules;
const base = all.filter((m) => !m.includes("/"));
check("builtin count", base.length === 37, String(base.length));
for (const name of base) check("resolves " + name, require("node:" + name) !== undefined);
// Every one of them imports as well as requires: the two paths are separate
// (the loader registers a module; require reads a table), and a name that
// only answers to one of them is a bug that hides until someone writes ESM.
for (const name of all) {
  const m = await import("node:" + name);
  check("imports " + name, m.default !== undefined || Object.keys(m).length > 0);
}

// child_process: a real process, its output, and its exit status.
const cp = require("node:child_process");
check("execSync", cp.execSync("echo hello").trim() === "hello");
check("execSync sees the shell's exit code", (() => {
  try { cp.execSync("exit 3"); return false; } catch (e) { return e.status === 3; }
})());
const sync = cp.spawnSync("/bin/echo", ["a", "b"]);
check("spawnSync stdout", sync.stdout === "a b\n", JSON.stringify(sync.stdout));
check("spawnSync status", sync.status === 0);
check("spawnSync on a missing file", cp.spawnSync("/no/such/bin", []).error !== undefined);
check("execFileSync", cp.execFileSync("/bin/echo", ["x"]).trim() === "x");
check("input reaches stdin", cp.spawnSync("/bin/cat", [], { input: "fed in" }).stdout === "fed in");
check("env is passed through", cp.execSync("echo $SXN_TEST_VAR", { env: { SXN_TEST_VAR: "set" } }).trim() === "set");
await new Promise((done) => {
  cp.exec("echo async", (error, stdout) => {
    check("exec callback", error === null && stdout.trim() === "async");
    done();
  });
});
await new Promise((done) => {
  const child = cp.spawn("/bin/echo", ["streamed"]);
  let out = "";
  child.stdout.on("data", (c) => { out += c; });
  child.on("close", (code) => {
    check("spawn streams and closes", out.trim() === "streamed" && code === 0);
    done();
  });
});

// dns: the system resolver, through libuv.
const dns = require("node:dns");
await new Promise((done) => dns.lookup("localhost", (e, address) => {
  check("dns.lookup", !e && (address === "127.0.0.1" || address === "::1"), address);
  done();
}));
await new Promise((done) => dns.lookup("nothing.invalid", (e) => {
  check("dns.lookup on a bad name", e && e.code === "ENOTFOUND");
  done();
}));
const dnsp = require("node:dns/promises");
check("dns.promises.lookup", typeof (await dnsp.lookup("localhost")).address === "string");
check("dns.resolve4", Array.isArray(await new Promise((r) => dns.resolve4("localhost", (e, a) => r(a || [])))));

// dgram: a real UDP round trip.
const dgram = require("node:dgram");
await new Promise((done) => {
  const server = dgram.createSocket("udp4");
  server.on("message", (msg, rinfo) => {
    check("udp message", msg.toString() === "ping" && rinfo.port > 0);
    server.close();
    done();
  });
  server.bind(0, () => {
    const client = dgram.createSocket("udp4");
    client.send("ping", server.address().port, "127.0.0.1", () => client.close());
  });
});

// vm, v8, punycode, diagnostics_channel: the ones that compute something.
const vm = require("node:vm");
check("vm.runInThisContext", vm.runInThisContext("40 + 2") === 42);
check("vm.runInNewContext sees the sandbox", vm.runInNewContext("return x * 2", { x: 21 }) === 42);
check("vm.Script", new vm.Script("7 * 6").runInThisContext() === 42);
const v8 = require("node:v8");
check("v8 heap statistics", v8.getHeapStatistics().used_heap_size > 0);
check("v8 serialize round trip", v8.deserialize(v8.serialize({ a: [1, 2] })).a[1] === 2);
const punycode = require("node:punycode");
check("punycode.toASCII", punycode.toASCII("münchen.de") === "xn--mnchen-3ya.de");
check("punycode.toUnicode", punycode.toUnicode("xn--mnchen-3ya.de") === "münchen.de");
check("punycode round trip", punycode.decode(punycode.encode("räksmörgås")) === "räksmörgås");
const dc = require("node:diagnostics_channel");
let published = null;
dc.subscribe("sxn:test", (m) => { published = m; });
check("channel has subscribers", dc.hasSubscribers("sxn:test"));
dc.channel("sxn:test").publish({ n: 1 });
check("diagnostics_channel delivers", published && published.n === 1);

// async_hooks: the store, which is the reason the module is here.
const { AsyncLocalStorage } = require("node:async_hooks");
const als = new AsyncLocalStorage();
check("store inside run", als.run({ id: 1 }, () => als.getStore().id) === 1);
check("store is gone after", als.getStore() === undefined);
check("store survives an await", await als.run({ id: 2 }, async () => {
  await Promise.resolve();
  return als.getStore().id === 2;
}));

// readline: lines out of a stream.
const readline = require("node:readline");
const { Readable } = require("node:stream");
const lines = [];
for await (const line of readline.createInterface({ input: Readable.from(["one\ntwo\nthree\n"]) }))
  lines.push(line);
check("readline splits lines", lines.join("|") === "one|two|three", lines.join("|"));

// stream/web is the global Web Streams, not a second implementation.
const web = require("node:stream/web");
check("stream/web is the same ReadableStream", web.ReadableStream === globalThis.ReadableStream);
check("stream/web has the compression streams", web.CompressionStream === globalThis.CompressionStream);

// The ones that answer a question rather than do work.
check("worker_threads.isMainThread", require("node:worker_threads").isMainThread === true);
check("cluster.isPrimary", require("node:cluster").isPrimary === true);
check("inspector.url", require("node:inspector").url() === undefined);
check("https.request exists", typeof require("node:https").request === "function");
check("http2 constants", require("node:http2").constants.HTTP2_HEADER_PATH === ":path");
check("console module logs", typeof require("node:console").log === "function");
check("constants has fs's", typeof require("node:constants").O_RDONLY === "number");

// What is not supported says so rather than failing obscurely.
for (const [name, call] of [
  ["worker_threads.Worker", () => new (require("node:worker_threads").Worker)("x")],
  ["cluster.fork", () => require("node:cluster").fork()],
  ["tls.connect", () => require("node:tls").connect({})],
  ["http2.connect", () => require("node:http2").connect("https://x.dev")],
  ["child_process.fork", () => require("node:child_process").fork("x")],
  ["https.createServer", () => require("node:https").createServer()],
]) {
  let message = "";
  try { call(); } catch (e) { message = e.message; }
  check(name + " explains itself", message.includes("not supported"), JSON.stringify(message));
}

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
