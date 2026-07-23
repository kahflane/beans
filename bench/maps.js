// bun maps.js — mirror of maps.b / maps.go (hash map throughput)
const n = Number(process.argv[2] ?? 400000);
const seed = Number(process.argv[3] ?? 1);
const m = new Map();
for (let i = 0; i < n; i++) m.set(i + seed, i * 2 + seed);

let sum = 0;
for (let i = 0; i < n; i++) sum += m.get(i + seed);

let hits = 0;
for (let i = 0; i < n; i++) {
    if (m.has(i + seed)) hits++;
    if (m.has(i + n + seed)) hits++;
}

const sn = Math.floor(n / 5);
const sm = new Map();
for (let i = 0; i < sn; i++) sm.set(`key${seed}_${i}`, i + seed);
let ssum = 0;
for (let i = 0; i < sn; i++) ssum += sm.get(`key${seed}_${i}`);

console.log(`maps ${sum} ${hits} ${ssum} ${m.size} ${sm.size}`);
