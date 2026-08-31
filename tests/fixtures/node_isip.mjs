// net.isIP, which is the system's address parser now (js_net_is_ip in
// src/node.c) rather than two regexps. The expected answers are Node's.
import net from "node:net";
import { readFileSync } from "node:fs";

const printed = [];
const console = { log: (...args) => printed.push(args.join(" ")) };
const cases = ["1.2.3.4", "0.0.0.0", "255.255.255.255", "256.1.1.1", "01.2.3.4", "1.2.3", "1.2.3.4.5",
  "", " ", "1.2.3.04", "::1", "::", "fe80::1", "fe80::1%eth0", "2001:db8::8a2e:370:7334", "::ffff:1.2.3.4",
  "1::2::3", "abcd", "1.2.3.4 ", " 1.2.3.4", "%eth0", "::1%", "12345::", "0:0:0:0:0:0:0:1", "1.2.3.-4",
  "1.2.3.4/24", "0x1.2.3.4", "999.999.999.999", "::ffff:0:0", "fe80::1%0"];
for (const c of cases) console.log(JSON.stringify(c), "isIP=" + net.isIP(c), "v4=" + net.isIPv4(c), "v6=" + net.isIPv6(c));
console.log("non-strings", net.isIP(42), net.isIP(null), net.isIP(undefined));

const expected = readFileSync(new URL("./node_isip.expected", import.meta.url).pathname, "utf8").trimEnd().split("\n");
let bad = 0;
for (let i = 0; i < Math.max(printed.length, expected.length); i++) {
  if (printed[i] === expected[i]) continue;
  bad++;
  globalThis.console.log("FAIL want " + (expected[i] ?? "(nothing)"));
  globalThis.console.log("      got " + (printed[i] ?? "(nothing)"));
}
globalThis.console.log(bad === 0 ? `net.isIP: ${printed.length} answers identical to Node` : `FAILURES: ${bad}`);
if (bad !== 0) process.exit(1);
