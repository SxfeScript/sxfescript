// Map/Set lookup must be sublinear. The integer hash once folded a double's
// two 32-bit halves together, which for small integers left the low bits --
// the bucket index -- nearly constant: 4096 consecutive integers landed in 8
// buckets, longest chain 1280, making has()/get() O(n). This asserts the
// shape of the cost curve rather than an absolute time, so it stays valid on
// slower machines and in Debug builds.
function perOp(n) {
  const s = new Set(), m = new Map();
  for (let i = 0; i < n; i++) { s.add(i); m.set(i, i); }
  const ITER = 100000;
  let c = 0;
  const t = performance.now();
  for (let i = 0; i < ITER; i++) { if (s.has(i % n)) c++; c += m.get(i % n) & 1; }
  return { ns: ((performance.now() - t) / ITER) * 1e6, c };
}
perOp(64); // warm
const small = perOp(64).ns;
const large = perOp(8192).ns;
const ratio = large / small;
// O(1) keeps this near 1; the O(n) regression measured a ratio above 15.
const ok = ratio < 4;
console.log(ok ? "PASS" : "FAIL",
            "small=" + small.toFixed(1) + "ns large=" + large.toFixed(1) + "ns ratio=" + ratio.toFixed(2));
// Object-identity keys hash the pointer, which is aligned and (with arena
// allocation) regularly spaced -- the same degeneracy as the numeric case.
function perOpObj(n) {
  const keep = [], s = new Set(), w = new WeakMap();
  for (let i = 0; i < n; i++) { const o = { i }; keep.push(o); s.add(o); w.set(o, i); }
  const ITER = 50000;
  let c = 0;
  const t = performance.now();
  for (let i = 0; i < ITER; i++) { const o = keep[i % n]; if (s.has(o)) c++; c += w.get(o) & 1; }
  return { ns: ((performance.now() - t) / ITER) * 1e6, c };
}
perOpObj(64);
const osmall = perOpObj(64).ns, olarge = perOpObj(8192).ns;
const oratio = olarge / osmall;
const ook = oratio < 4;
console.log(ook ? "PASS" : "FAIL",
            "obj small=" + osmall.toFixed(1) + "ns large=" + olarge.toFixed(1) + "ns ratio=" + oratio.toFixed(2));

// SameValueZero must survive any hash change.
const checks = [
  new Set([0]).has(-0), new Set([-0]).has(0), new Set([NaN]).has(NaN),
  new Set([1]).has(1.0), new Map([[1, "x"]]).get(1.0) === "x",
  new Map([[0, "a"]]).get(-0) === "a",
];
console.log(checks.every(Boolean) ? "PASS samevaluezero" : "FAIL samevaluezero " + JSON.stringify(checks));
if (!ok || !ook || !checks.every(Boolean)) throw new Error("map/set hash regression");
