// bun churn.js — mirror of churn.b / churn.go
const keep = [];
const n = Number(process.argv[2] ?? 5_000_000);
const seed = Number(process.argv[3] ?? 1);
let sum = 0;
for (let i = 0; i < n; i++) {
    const p = { a: i + seed, b: i + seed + 1 };
    const q = p;
    sum += q.a + p.b;
    if (i % 1000 === 0) keep.push(p);
}
console.log(`sum ${sum} kept ${keep.length}`);
