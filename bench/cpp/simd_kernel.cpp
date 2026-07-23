#include "common.h"

#include <cmath>
#include <cstdint>
#include <iostream>

using Float4 = float __attribute__((ext_vector_type(4)));

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 200000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    Float4 value = {static_cast<float>(seed), static_cast<float>(seed + 1),
                    static_cast<float>(seed + 2), static_cast<float>(seed + 3)};
    const Float4 scale = {0.99991f, 0.99989f, 0.99987f, 0.99983f};
    const Float4 delta = {0.011f, 0.013f, 0.017f, 0.019f};
    for (std::int64_t index = 0; index < n; ++index) value = value * scale + delta;
    const float sum = (value[0] + value[1]) + (value[2] + value[3]);
    std::cout << "simd_kernel " << std::llround(value[0]) << ' '
              << std::llround(value[1]) << ' ' << std::llround(value[2]) << ' '
              << std::llround(value[3]) << ' ' << std::llround(sum) << '\n';
}
