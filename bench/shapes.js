// bun shapes.js — mirror of shapes.b / shapes.go (virtual dispatch)
class Circle {
    constructor(r) { this.r = r; }
    area() { return 3.14159265 * this.r * this.r; }
}
class Square {
    constructor(s) { this.s = s; }
    area() { return this.s * this.s; }
}

const shapes = [new Circle(1.5), new Square(2.0)];
let total = 0;
for (let i = 0; i < 50_000_000; i++) {
    // indexed loop: the fast-path idiom for hot JS array walks
    for (let j = 0; j < shapes.length; j++) total += shapes[j].area();
}
console.log(Math.round(total));
