// bun fib.js — mirror of fib.b / fib.go
function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
const n = Number(process.argv[2] ?? 40);
const seed = Number(process.argv[3] ?? 1);
console.log(fib(n) + seed - seed);
