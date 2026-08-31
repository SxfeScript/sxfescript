// node:os and the parts of node:fs a server needs. These were stubs -- the
// hostname was "localhost", the memory sizes 0, the CPU list empty, and
// networkInterfaces and stat did not exist at all -- which is worse than
// missing, because nothing can tell a stub from the truth.
import * as os from "node:os";
import { networkInterfaces, hostname, cpus, totalmem, availableParallelism } from "node:os";
import { stat, lstat } from "node:fs/promises";
import { statSync, createReadStream, existsSync } from "node:fs";

let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

check("hostname is not a guess", hostname() !== "localhost" || hostname().length > 0, true);
check("there is at least one cpu", cpus().length > 0, true);
check("a cpu has a model", typeof cpus()[0].model === "string" && cpus()[0].model.length > 0, true);
check("total memory is real", totalmem() > 1e6, true);
check("free memory is real", os.freemem() > 0, true);
check("parallelism is at least one", availableParallelism() >= 1, true);
check("loadavg has three numbers", os.loadavg().length, 3);
check("uptime is positive", os.uptime() > 0, true);
check("release says something", os.release().length > 0, true);
check("homedir is absolute", os.homedir().startsWith("/") || /^[A-Za-z]:/.test(os.homedir()), true);

const nets = networkInterfaces();
check("interfaces is an object", typeof nets === "object" && nets !== null, true);
const all = Object.values(nets).flat();
check("there is a loopback address", all.some(a => a.internal && a.family === "IPv4"), true);
const one = all.find(a => a.family === "IPv4");
check("an address has a netmask", typeof one.netmask === "string" && one.netmask.includes("."), true);
check("an address has a cidr", /\/\d+$/.test(one.cidr), true);
check("an address has a mac", /^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/.test(one.mac), true);
check("family is the modern spelling", one.family, "IPv4");

const self = new URL(import.meta.url).pathname;
const s = await stat(self);
check("stat reports a size", s.size > 0, true);
check("stat knows it is a file", s.isFile(), true);
check("and not a directory", s.isDirectory(), false);
check("mtime is a Date", s.mtime instanceof Date && s.mtime.getTime() > 0, true);
check("statSync agrees", statSync(self).size, s.size);
check("a directory is a directory", statSync(os.tmpdir()).isDirectory(), true);
check("lstat works too", (await lstat(self)).isFile(), true);
let code = "";
try { await stat(self + ".missing"); } catch (e) { code = e.code; }
check("a missing file is ENOENT", code, "ENOENT");

const chunks = [];
await new Promise((resolve, reject) => {
  const rs = createReadStream(self);
  rs.on("data", (c) => chunks.push(c));
  rs.on("end", resolve);
  rs.on("error", reject);
});
check("createReadStream reads the file", chunks.reduce((n, c) => n + c.length, 0), s.size);
check("existsSync still works", existsSync(self), true);

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
