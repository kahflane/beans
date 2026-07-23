// bun shapes.js — mirror of shapes.b / shapes.go (virtual dispatch)
class Circle {
    constructor(r) { this.r = r; }
    area() { return 3.14159265 * this.r * this.r; }
}
class Square {
    constructor(s) { this.s = s; }
    area() { return this.s * this.s; }
}

const n = Number(process.argv[2] ?? 100_000_000);
const seed = Number(process.argv[3] ?? 1);
const tweak = (seed % 7) / 100;
const shapeCount = 8 + seed % 8;
const shapes = [];
for (let i = 0; i < shapeCount; i++) {
    shapes.push(i % 2 === 0 ? new Circle(1.5 + tweak) : new Square(2.0 + tweak));
}
let total = 0;
for (let i = 0; i < Math.floor(n / shapeCount); i++) {
    // indexed loop: the fast-path idiom for hot JS array walks
    for (let j = 0; j < shapes.length; j++) total += shapes[j].area();
}
console.log(Math.round(total));
