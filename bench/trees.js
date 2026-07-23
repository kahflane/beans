// bun trees.js — mirror of trees.b / trees.go (binary-trees)
function build(depth, seed) {
    const n = { left: null, right: null, value: depth + seed };
    if (depth > 0) {
        n.left = build(depth - 1, seed);
        n.right = build(depth - 1, seed);
    }
    return n;
}

function count(n) {
    let total = 1 + n.value;
    if (n.left !== null) total += count(n.left);
    if (n.right !== null) total += count(n.right);
    return total;
}

const maxDepth = Number(process.argv[2] ?? 14);
const seed = Number(process.argv[3] ?? 1);
const longLived = build(maxDepth, seed);
let total = 0;

for (let d = 4; d <= maxDepth; d += 2) {
    let iters = 1;
    for (let k = maxDepth - d + 4; k > 0; k--) iters *= 2;
    for (let i = 0; i < iters; i++) total += count(build(d, seed));
}

console.log(`trees ${total} long ${count(longLived)}`);
