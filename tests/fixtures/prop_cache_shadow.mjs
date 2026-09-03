// The property cache is invalidated when a prototype gains a property, and
// not when an ordinary object does. These are the shapes where the second
// half could go wrong: a site that has already cached where it found a
// property, and then something adds one that should shadow it, or moves it.
//
// Every answer here is the language's, and none of them depends on a cache.

let failures = 0;
function check(name, actual, expected) {
  if (!Object.is(actual, expected)) {
    console.log("FAIL", name, "expected", expected, "got", actual);
    failures += 1;
  }
}
// One call site per case, so each has its own cache entry, and each is warmed
// before the mutation it is meant to notice.
const warm = (f, x, n) => { for (let i = 0; i < n; i++) f(x); };

// --- a new own property shadows an inherited one -------------------------
{
  const proto = { m: "proto" };
  const o = Object.create(proto);
  const read = (x) => x.m;
  warm(read, o, 200);
  check("inherited before", read(o), "proto");
  o.m = "own";
  check("own shadows inherited", read(o), "own");
}

// --- a nearer prototype shadows a further one ----------------------------
{
  const far = { m: "far" };
  const near = Object.create(far);
  const o = Object.create(near);
  const read = (x) => x.m;
  warm(read, o, 200);
  check("found at depth 2", read(o), "far");
  near.m = "near";
  check("depth 1 shadows depth 2", read(o), "near");
}

// --- Object.prototype, reached from a plain object -----------------------
{
  const o = { a: 1 };
  const read = (x) => x.zz_probe;
  check("absent before", read(o), undefined);
  warm(read, o, 200);
  Object.prototype.zz_probe = "proto";
  check("added to Object.prototype", read(o), "proto");
  const mid = { zz_probe: "own" };
  check("own beats Object.prototype", read(mid), "own");
  delete Object.prototype.zz_probe;
  check("deleted again", read(o), undefined);
}

// --- a constructor prototype gaining a method ----------------------------
{
  function Foo() { this.a = 1; }
  Foo.prototype.m = function () { return "one"; };
  const f = new Foo();
  const call = (x) => x.m();
  warm(call, f, 200);
  check("prototype method", call(f), "one");
  Foo.prototype.m = function () { return "two"; };
  check("replaced on the prototype", call(f), "two");
  f.m = function () { return "own"; };
  check("own method shadows it", call(f), "own");
}

// --- a fresh object assigned as a prototype after the site is warm -------
{
  function Bar() {}
  const b = new Bar();
  const read = (x) => x.k;
  Object.prototype.k = "object";
  warm(read, b, 200);
  check("from Object.prototype", read(b), "object");
  Bar.prototype.k = "bar";
  check("Bar.prototype shadows it", read(b), "bar");
  delete Object.prototype.k;
  check("still Bar's", read(b), "bar");
}

// --- setPrototypeOf under a warm site ------------------------------------
{
  const a = { m: "a" }, c = { m: "c" };
  const o = Object.create(a);
  const read = (x) => x.m;
  warm(read, o, 200);
  check("through a", read(o), "a");
  Object.setPrototypeOf(o, c);
  check("through c", read(o), "c");
}

// --- ordinary objects growing, which must not disturb anything ----------
{
  const proto = { m: "proto" };
  const read = (x) => x.m;
  const first = Object.create(proto);
  warm(read, first, 200);
  let last = null;
  for (let i = 0; i < 500; i++) {
    // Each of these adds four properties to an ordinary object, which is
    // what used to flush the cache 2000 times over this loop.
    const churn = { w: i, x: i, y: i, z: i };
    if (churn.w !== i) { failures += 1; }
    last = Object.create(proto);
    last.own = i;
    if (read(last) !== "proto") { failures += 1; }
  }
  check("still found through the prototype", read(first), "proto");
  check("and on the last one built", read(last), "proto");
  proto.m = "changed";
  check("changing the prototype is still seen", read(first), "changed");
}

// --- accessors and non-writables on a prototype --------------------------
{
  const proto = {};
  Object.defineProperty(proto, "g", { get() { return "getter"; }, configurable: true });
  const o = Object.create(proto);
  const read = (x) => x.g;
  warm(read, o, 200);
  check("inherited getter", read(o), "getter");
  Object.defineProperty(proto, "g", { value: "data", configurable: true });
  check("redefined as data", read(o), "data");
}

// --- delete, resize and compaction --------------------------------------
{
  const o = {};
  for (let i = 0; i < 40; i++) o["p" + i] = i;
  const read = (x) => x.p39;
  warm(read, o, 200);
  check("last property", read(o), 39);
  for (let i = 0; i < 39; i++) delete o["p" + i];
  check("survives 39 deletes", read(o), 39);
  for (let i = 0; i < 40; i++) o["q" + i] = i;
  check("survives regrowth", read(o), 39);
}

if (failures !== 0) throw new Error(failures + " property-cache checks failed");
console.log("property cache: all checks passed");
