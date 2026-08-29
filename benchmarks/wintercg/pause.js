const N = 2000000;
let worst = 0, total = 0;
const t0 = performance.now();
let last = t0;
for (let i = 0; i < N; i++) {
  total += Buffer.from("payload " + i, "utf-8").length;
  const now = performance.now();
  if (now - last > worst) worst = now - last;
  last = now;
}
console.log("total:", (performance.now() - t0).toFixed(1), "ms  worst pause:", worst.toFixed(2), "ms");
