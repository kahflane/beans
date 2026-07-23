#include "common.h"

#include <cstdint>
#include <iostream>

template <class T>
static T choose(T left, T right, bool use_left) {
    return use_left ? left : right;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 400000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < n; ++i)
        sum += choose((i + seed) % 101, (i + seed) % 67, i % 2 == 0);
    std::cout << "generic_calls " << sum << '\n';
}
