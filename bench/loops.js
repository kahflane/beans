// bun loops.js — mirror of loops.b / loops.go
let sum = 0;
for (let i = 1; i <= 200_000_000; i++) {
    sum += i % 7;
}
console.log(sum);
