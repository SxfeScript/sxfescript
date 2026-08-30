interface P { n: number }
const f = (p: P): number => p.n * 2;
export const r = f({n: 21});
console.log("ts entry:", r);
