const N = 2000000;
let worst = 0, over10us = 0, over100us = 0;
const t0 = performance.now(); let last = t0;
const live = new Array(2000);
for (let i = 0; i < N; i++) {
  live[i % 2000] = { i, s: "payload " + i };
  const now = performance.now();
  const d = now - last;
  if (d > worst) worst = d;
  if (d > 0.01) over10us++;
  if (d > 0.1) over100us++;
  last = now;
}
console.log("total:", (performance.now()-t0).toFixed(1), "ms  worst:", worst.toFixed(3), "ms  >10us:", over10us, " >100us:", over100us);
