#include "common.h"

#include <cstdint>
#include <iostream>

static std::int64_t step(std::int64_t value, std::int64_t seed) {
    const auto mixed = (value * 17 + seed) % 1000003;
    return mixed % 3 == 0 ? mixed / 3 : mixed + 11;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 40000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < n; ++i) sum += step(i + sum % 97, seed);
    std::cout << "direct_calls " << sum << '\n';
}
