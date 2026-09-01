// A seeded random walk over JSON, to keep the fast paths in JSON.parse and
// JSON.stringify honest: documents are generated, written, parsed back and
// written again, and every intermediate string is hashed. Running this under
// Node gives the same hash, so a divergence anywhere shows up as one number.
// Deterministic: same seed, same documents, same hash.

let state = 0x2f6e2b1;
const rand = () => {
  // xorshift32, so Node and this runtime walk identical documents.
  state ^= state << 13; state >>>= 0;
  state ^= state >>> 17;
  state ^= state << 5; state >>>= 0;
  return state;
};
const pick = (n) => rand() % n;

const KEYS = ["id", "name", "ok", "value", "", "__proto__", "toJSON", "a b", 'q"q', "ключ", "🙂",
              "aVeryLongPropertyNameThatGoesPastTheShortStringCases", "0", "1", "12", "-1", "1e3"];
const STRINGS = ["", "plain", "with \"quote\"", "back\\slash", "tab\there", "new\nline",
                 "controlchar", "del", "café", "日本語", "🎉 emoji", "\ud800 lone high",
                 "\udc00 lone low", "x".repeat(200), "12345678\"boundary", "1234567é8"];
const NUMBERS = [0, -0, 1, -1, 42, 2147483647, -2147483648, 2147483648, 9007199254740991,
                 1e21, 1e-7, 0.1, -2.25, 1.7976931348623157e308, 5e-324, 123456789012345678901234567890];

function value(depth) {
  switch (pick(depth > 3 ? 5 : 8)) {
    case 0: return null;
    case 1: return pick(2) === 0;
    case 2: return NUMBERS[pick(NUMBERS.length)];
    case 3: return STRINGS[pick(STRINGS.length)];
    case 4: return rand() / 1000;
    case 5: {
      const n = pick(6);
      const out = [];
      for (let i = 0; i < n; i++) out.push(value(depth + 1));
      return out;
    }
    default: {
      const n = pick(6);
      const out = {};
      for (let i = 0; i < n; i++) out[KEYS[pick(KEYS.length)]] = value(depth + 1);
      return out;
    }
  }
}

// FNV-1a over every string produced, so one number covers every document.
let hash = 0x811c9dc5;
const feed = (s) => {
  for (let i = 0; i < s.length; i++) {
    hash ^= s.charCodeAt(i) & 0xff;
    hash = Math.imul(hash, 0x01000193) >>> 0;
    hash ^= s.charCodeAt(i) >>> 8;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
};

let documents = 0;
for (let i = 0; i < 4000; i++) {
  const v = value(0);
  const text = JSON.stringify(v);
  if (text === undefined) continue;
  feed(text);
  const back = JSON.parse(text);
  const again = JSON.stringify(back);
  feed(again);
  if (again !== text) {
    console.log("FAIL round trip differs at document", i);
    console.log("  first :", text.slice(0, 200));
    console.log("  second:", again.slice(0, 200));
    process.exit(1);
  }
  // The same document with an indent, which takes the other separator path.
  feed(JSON.stringify(v, null, 2));
  // And through a reviver and a replacer, which take the general paths.
  feed(JSON.stringify(JSON.parse(text, (k, x) => x)));
  feed(JSON.stringify(v, (k, x) => x));
  documents++;
}

// Node's hash for this seed. It is the whole point of the file: if anything
// about parsing or writing changes by one byte, this stops matching.
const EXPECTED = "edf3782d";
console.log("documents:", documents);
console.log("hash:", hash.toString(16));
if (hash.toString(16) !== EXPECTED) {
  console.log("FAIL hash differs from Node's", hash.toString(16), "want", EXPECTED);
  process.exit(1);
}
console.log("ALL PASS");
