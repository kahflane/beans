// bun maps.js — mirror of maps.b / maps.go (hash map throughput)
const n = 400000;
const m = new Map();
for (let i = 0; i < n; i++) m.set(i, i * 2);

let sum = 0;
for (let i = 0; i < n; i++) sum += m.get(i);

let hits = 0;
for (let i = 0; i < n; i++) {
    if (m.has(i)) hits++;
    if (m.has(i + n)) hits++;
}

const sn = 80000;
const sm = new Map();
for (let i = 0; i < sn; i++) sm.set(`key${i}`, i);
let ssum = 0;
for (let i = 0; i < sn; i++) ssum += sm.get(`key${i}`);

console.log(`maps ${sum} ${hits} ${ssum} ${m.size} ${sm.size}`);
