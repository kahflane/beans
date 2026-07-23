#include "common.h"

#include <cstdint>
#include <iostream>
#include <thread>

static std::int64_t mix(std::int64_t start, std::int64_t stop, std::int64_t seed) {
    std::int64_t sum = 0;
    for (auto i = start; i < stop; ++i) sum += (i + seed) % 7;
    return sum;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
    std::thread worker([&] { sum = mix(0, n, seed); });
    worker.join();
    std::cout << "parallel_1 " << sum << '\n';
}
