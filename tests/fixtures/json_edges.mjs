// JSON.parse and JSON.stringify around the edges of the fast paths added for
// speed: an escape-free run ends exactly at a quote, a backslash, a control
// character or a non-ASCII byte, and the scan works a word at a time, so the
// interesting cases sit at those boundaries.
let bad = 0;
const check = (n, got, want) => { const ok = got === want; if (!ok) bad++;
  console.log((ok?"ok   ":"FAIL ") + n + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); };

const cases = [
  ["plain", "hello world"],
  ["empty", ""],
  ["quote", 'a"b'],
  ["backslash", "a\\b"],
  ["newline", "a\nb"],
  ["tab", "a\tb"],
  ["control", "ab"],
  ["del", "ab"],
  ["latin1", "café"],
  ["cjk", "日本語"],
  ["emoji", "🎉 done"],
  ["lone high surrogate", "a\ud800b"],
  ["lone low surrogate", "a\udc00b"],
  ["pair", "😀"],
  ["long ascii", "x".repeat(1000)],
  ["escape at 7", '1234567"tail'],
  ["escape at 8", '12345678"tail'],
  ["escape at 9", '123456789"tail'],
  ["non-ascii at 8", "12345678é9"],
  ["control at 8", "123456789"],
  ["backslash at 8", "12345678\\9"],
];
for (const [name, value] of cases) {
  check(`round trip ${name}`, JSON.parse(JSON.stringify(value)), value);
}

check("quote escape", JSON.stringify('a"b'), '"a\\"b"');
check("backslash escape", JSON.stringify("a\\b"), '"a\\\\b"');
check("control escape", JSON.stringify(""), '"\\u0001"');
check("lone surrogate escape", JSON.stringify("\ud800"), '"\\ud800"');
check("emoji stays whole", JSON.stringify("🎉"), '"🎉"');
check("tab escape", JSON.stringify("\t"), '"\\t"');
check("del is not escaped", JSON.stringify(""), '""');

check("parse \\u", JSON.parse('"\\u0041\\u00e9"'), "Aé");
check("parse escapes", JSON.parse('"a\\nb\\tc\\\\d\\"e\\/f"'), 'a\nb\tc\\d"e/f');
check("parse utf8", JSON.parse('"日本語"'), "日本語");

// Numbers: the integer fast path has to agree with strtod exactly.
const numbers = ["0", "-0", "1", "-1", "42", "2147483647", "-2147483648",
                 "2147483648", "-2147483649", "999999999999999999",
                 "1000000000000000000", "9007199254740993",
                 "1.5", "-2.25", "1e3", "1E-3", "1.7976931348623157e308",
                 "5e-324", "0.1", "123456789012345678901234567890"];
for (const n of numbers) check(`number ${n}`, JSON.parse(n), Number(n));
check("negative zero keeps its sign", 1 / JSON.parse("-0"), -Infinity);

const doc = JSON.parse('{"a":[1,2,{"b":"c"}],"d":null,"e":true}');
check("nested", JSON.stringify(doc), '{"a":[1,2,{"b":"c"}],"d":null,"e":true}');
check("key with escape", JSON.stringify({ 'a"b': 1 }), '{"a\\"b":1}');
check("reviver", JSON.parse('{"n":2}', (k, v) => typeof v === "number" ? v * 3 : v).n, 6);
check("replacer", JSON.stringify({ a: 1, b: 2 }, ["a"]), '{"a":1}');
check("indent", JSON.stringify({ a: 1 }, null, 2), '{\n  "a": 1\n}');
check("toJSON gets the key", JSON.stringify({ k: { toJSON: (key) => key } }), '{"k":"k"}');

for (const text of ['{"a":}', '"unterminated', '"ab"', "[1,]", "01", "1.", "+1", "'x'"]) {
  let threw = false;
  try { JSON.parse(text); } catch { threw = true; }
  check(`rejects ${JSON.stringify(text)}`, threw, true);
}

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
