// bun mandel.js — mirror of mandel.b / mandel.go (float math, no allocation)
const size = Number(process.argv[2] ?? 1800);
const seed = Number(process.argv[3] ?? 1);
const w = size;
const h = size;
const maxIter = 80 + (seed % 41);
let inside = 0;

for (let y = 0; y < h; y++) {
    const ci = (y * 2.0) / h - 1.0;
    for (let x = 0; x < w; x++) {
        const cr = (x * 3.0) / w - 2.0;
        let zr = 0.0;
        let zi = 0.0;
        let i = 0;
        for (; i < maxIter; i++) {
            const t = zr * zr - zi * zi + cr;
            zi = 2.0 * zr * zi + ci;
            zr = t;
            if (zr * zr + zi * zi > 4.0) break;
        }
        if (i === maxIter) inside++;
    }
}

console.log(`mandel ${inside}`);
