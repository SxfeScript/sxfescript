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

// stringify caches what it learns about an object's shape; anything that
// changes where a property lives has to invalidate that.
check("toJSON on a shape seen before", JSON.stringify([{ a: 1 }, { a: 1, toJSON: () => "x" }]), '[{"a":1},"x"]');
{
  // A getter installs toJSON on Object.prototype after an object of the same
  // shape has already been written; the ones after it must pick it up.
  const doc = {
    first: { a: 1 },
    hook: { get a() { Object.prototype.toJSON = function () { return "late"; }; return 0; } },
    second: { a: 2 },
  };
  const out = JSON.stringify(doc);
  delete Object.prototype.toJSON;
  check("toJSON installed mid-run", out, '{"first":{"a":1},"hook":{"a":0},"second":"late"}');
}
{
  const seen = { a: { x: 1 }, b: { x: 2 } };
  Object.prototype.toJSON = function () { return "P"; };
  const out = JSON.stringify(seen);
  delete Object.prototype.toJSON;
  check("toJSON from the prototype", out, '"P"');
}
{
  // Two proxies with different traps but the same (empty) shape: what the
  // first one answers about toJSON must not be assumed of the second.
  const p1 = new Proxy({ a: 1 }, { get(t, k) { return k === "toJSON" ? undefined : t[k]; } });
  const p2 = new Proxy({ a: 1 }, { get(t, k) { return k === "toJSON" ? () => "second" : t[k]; } });
  check("proxies are not one shape", JSON.stringify([p1, p2]), '[{"a":1},"second"]');
}
{
  // Two objects of the same shape, one of which has a toJSON value stored
  // into the slot the other left undefined. A shape says which properties
  // exist, not what they hold.
  const make = () => ({ x: 1, toJSON: undefined });
  const a = make(), b = make();
  b.toJSON = () => "hijacked";
  check("same shape, different toJSON", JSON.stringify([a, b]), '[{"x":1},"hijacked"]');
}
{
  // The same through a prototype: the value is replaced in place, which
  // moves nothing.
  const proto = { toJSON: undefined };
  const a = Object.create(proto), b = Object.create(proto);
  a.x = 1; b.x = 2;
  const first = JSON.stringify(a);
  proto.toJSON = () => "from proto";
  check("a prototype's toJSON replaced in place", first + JSON.stringify(b), '{"x":1}"from proto"');
}
// The key list is taken once, before any getter runs: a getter that changes
// another key's enumerability cannot change what is written.
check("enumerability is a snapshot",
      JSON.stringify({ get a() { Object.defineProperty(this, "b", { value: 2, enumerable: false }); return 1; }, b: 2 }),
      '{"a":1,"b":2}');
check("a key made enumerable mid-run",
      JSON.stringify(Object.defineProperties({}, {
        a: { enumerable: true, get() { Object.defineProperty(this, "b", { enumerable: true }); return 1; } },
        b: { value: 2, enumerable: false, configurable: true },
      })),
      '{"a":1}');
check("__proto__ is an own key", JSON.stringify(JSON.parse('{"__proto__":{"x":1}}')), '{"__proto__":{"x":1}}');
check("__proto__ does not set the prototype", Object.getPrototypeOf(JSON.parse('{"__proto__":{"x":1}}')) === Object.prototype, true);
check("non-enumerable keys are skipped", JSON.stringify(Object.defineProperty({ a: 1 }, "b", { value: 2 })), '{"a":1}');
check("a getter's value is used", JSON.stringify({ get a() { return 7; } }), '{"a":7}');
check("numeric keys sort first", JSON.stringify({ b: 1, 2: 2, a: 3, 1: 4 }), '{"1":4,"2":2,"b":1,"a":3}');
check("symbol keys are skipped", JSON.stringify({ [Symbol("s")]: 1, a: 2 }), '{"a":2}');
check("inherited keys are skipped", JSON.stringify(Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true } })), '{"own":2}');
check("a key deleted by a getter", JSON.stringify({ get a() { delete this.b; return 1; }, b: 2 }), '{"a":1}');
check("array holes are null", JSON.stringify([1, , 3]), '[1,null,3]');
check("Date uses its toJSON", JSON.stringify({ d: new Date(0) }), '{"d":"1970-01-01T00:00:00.000Z"}');

for (const text of ['{"a":}', '"unterminated', '"ab"', "[1,]", "01", "1.", "+1", "'x'"]) {
  let threw = false;
  try { JSON.parse(text); } catch { threw = true; }
  check(`rejects ${JSON.stringify(text)}`, threw, true);
}

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
if (bad !== 0) throw new Error(bad + " checks failed");
