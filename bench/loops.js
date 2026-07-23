// bun loops.js — mirror of loops.b / loops.go
let sum = 0;
const n = Number(process.argv[2] ?? 200_000_000);
const seed = Number(process.argv[3] ?? 1);
for (let i = 1; i <= n; i++) {
    sum += (i + seed) % 7;
}
console.log(sum);
