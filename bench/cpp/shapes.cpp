#include "common.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

struct Circle final : Shape {
    explicit Circle(double value) : r(value) {}
    double area() const override { return 3.14159265 * r * r; }
    double r;
};

struct Square final : Shape {
    explicit Square(double value) : s(value) {}
    double area() const override { return s * s; }
    double s;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 100000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    const double tweak = static_cast<double>(seed % 7) / 100.0;
    const auto shape_count = 8 + seed % 8;
    double total = 0.0;
#ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<Shape>> shapes;
    shapes.reserve(static_cast<std::size_t>(shape_count));
    for (std::int64_t i = 0; i < shape_count; ++i) {
        if (i % 2 == 0) shapes.push_back(std::make_shared<Circle>(1.5 + tweak));
        else shapes.push_back(std::make_shared<Square>(2.0 + tweak));
    }
#else
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.reserve(static_cast<std::size_t>(shape_count));
    for (std::int64_t i = 0; i < shape_count; ++i) {
        if (i % 2 == 0) shapes.push_back(std::make_unique<Circle>(1.5 + tweak));
        else shapes.push_back(std::make_unique<Square>(2.0 + tweak));
    }
#endif
    const auto rounds = n / shape_count;
    for (std::int64_t i = 0; i < rounds; ++i)
        for (const auto& shape : shapes) total += shape->area();
    std::cout << static_cast<std::int64_t>(std::round(total)) << '\n';
}
