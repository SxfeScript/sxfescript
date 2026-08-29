// Config/manifest parsing: JSON round-trip over a realistic nested payload.
// Shape of app startup, CI tooling, and serverless cold paths.
const pkg = { name:"app", version:"1.2.3", deps:{}, scripts:{}, files:[] };
for (let i=0;i<200;i++){ pkg.deps["pkg-"+i]="^"+i+".0.0"; pkg.scripts["task"+i]="run "+i; pkg.files.push("src/f"+i+".ts"); }
const t0=performance.now(); let n=0;
for (let i=0;i<2000;i++){ const s=JSON.stringify(pkg); const o=JSON.parse(s); n+=Object.keys(o.deps).length; }
console.log("config:", (performance.now()-t0).toFixed(1), "ms", n);
