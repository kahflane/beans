// bun trees.js — mirror of trees.b / trees.go (binary-trees)
function build(depth) {
    const n = { left: null, right: null };
    if (depth > 0) {
        n.left = build(depth - 1);
        n.right = build(depth - 1);
    }
    return n;
}

function count(n) {
    let total = 1;
    if (n.left !== null) total += count(n.left);
    if (n.right !== null) total += count(n.right);
    return total;
}

const maxDepth = 14;
const longLived = build(maxDepth);
let total = 0;

for (let d = 4; d <= maxDepth; d += 2) {
    let iters = 1;
    for (let k = maxDepth - d + 4; k > 0; k--) iters *= 2;
    for (let i = 0; i < iters; i++) total += count(build(d));
}

console.log(`trees ${total} long ${count(longLived)}`);
