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
    const auto half = n / 2;
    std::int64_t left = 0, right = 0;
    std::thread t0([&] { left = mix(0, half, seed); });
    std::thread t1([&] { right = mix(half, n, seed); });
    t0.join(); t1.join();
    std::cout << "parallel_2 " << left + right << '\n';
}
