// bun churn.js — mirror of churn.b / churn.go
const keep = [];
let sum = 0;
for (let i = 0; i < 5000000; i++) {
    const p = { a: i, b: i + 1 };
    const q = p;
    sum += q.a + p.b;
    if (i % 1000 === 0) keep.push(p);
}
console.log(`sum ${sum} kept ${keep.length}`);
