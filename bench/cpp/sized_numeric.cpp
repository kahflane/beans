#include "common.h"

#include <cmath>
#include <cstdint>
#include <iostream>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 100000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int8_t a = static_cast<std::int8_t>(seed);
    std::uint16_t b = static_cast<std::uint16_t>(seed);
    std::uint32_t c = static_cast<std::uint32_t>(seed);
    std::uint64_t d = static_cast<std::uint64_t>(seed);
    float f = static_cast<float>(seed) / 10.0f;
    for (std::int64_t i = 0; i < n; ++i) {
        a = static_cast<std::int8_t>(a + 17);
        b = static_cast<std::uint16_t>(b * 3u + 1u);
        c = c * 1664525u + 1013904223u;
        d = d * UINT64_C(6364136223846793005) + 1u;
        f = f * 0.9999f + 0.0003f;
    }
    std::cout << "sized_numeric " << static_cast<int>(a) << ' ' << b << ' ' << c
              << ' ' << d << ' ' << std::llround(f) << '\n';
}
