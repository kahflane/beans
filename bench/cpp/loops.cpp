#include "common.h"

#include <cstdint>
#include <iostream>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 200000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
    for (std::int64_t i = 1; i <= n; ++i) sum += (i + seed) % 7;
    std::cout << sum << '\n';
}
