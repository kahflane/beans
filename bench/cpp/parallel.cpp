#include "common.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <thread>

static std::int64_t mix(std::int64_t start, std::int64_t stop,
                        std::int64_t seed) {
    std::int64_t sum = 0;
    for (auto i = start; i < stop; ++i) sum += (i + seed) % 7;
    return sum;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    const auto q = n / 4;
    std::array<std::int64_t, 4> sums{};
    std::array<std::thread, 4> threads = {
        std::thread([&] { sums[0] = mix(0, q, seed); }),
        std::thread([&] { sums[1] = mix(q, q * 2, seed); }),
        std::thread([&] { sums[2] = mix(q * 2, q * 3, seed); }),
        std::thread([&] { sums[3] = mix(q * 3, n, seed); }),
    };
    for (auto& thread : threads) thread.join();
    std::cout << "parallel " << sums[0] + sums[1] + sums[2] + sums[3] << '\n';
}
