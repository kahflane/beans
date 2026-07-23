// bun strings.js — mirror of strings.b / strings.go (string build + scan)
const n = Number(process.argv[2] ?? 2000000);
const seed = Number(process.argv[3] ?? 1);

const parts = [];
for (let i = 0; i < n; i++) parts.push(`item${seed}_${i}`);

const joined = parts.join(",");
const up = joined.toUpperCase();
const back = joined.split(",");

let hits = 0;
for (let i = 0; i < back.length; i++) {
    if (back[i].includes("999")) hits++;
}

const swapped = joined.replaceAll("item", "row");

console.log(
    `strings ${joined.length} ${up.length} ${back.length} ${hits} ${swapped.length}`,
);
