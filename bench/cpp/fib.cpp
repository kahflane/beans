#include "common.h"

#include <cstdint>
#include <iostream>

static std::int64_t fib(std::int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 40);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::cout << fib(n) + seed - seed << '\n';
}
