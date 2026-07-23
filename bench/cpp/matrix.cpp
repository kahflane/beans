#include "common.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1600);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<double> matrix;
    matrix.reserve(static_cast<std::size_t>(n * n));
    for (std::int64_t row = 0; row < n; ++row)
        for (std::int64_t col = 0; col < n; ++col)
            matrix.push_back(static_cast<double>((row * 17 + col * 31 + seed) % 1000) /
                             1000.0);
    std::vector<double> input, output;
    input.reserve(static_cast<std::size_t>(n));
    output.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        input.push_back(static_cast<double>((i + seed) % 97));
        output.push_back(0.0);
    }
    for (int round = 0; round < 30; ++round) {
        for (std::int64_t row = 0; row < n; ++row) {
            double total = 0.0;
            for (std::int64_t col = 0; col < n; ++col) {
#ifdef BEANS_MATCHED
                total += matrix.at(static_cast<std::size_t>(row * n + col)) *
                         input.at(static_cast<std::size_t>(col));
#else
                total += matrix[static_cast<std::size_t>(row * n + col)] *
                         input[static_cast<std::size_t>(col)];
#endif
            }
#ifdef BEANS_MATCHED
            output.at(static_cast<std::size_t>(row)) = total / static_cast<double>(n);
#else
            output[static_cast<std::size_t>(row)] = total / static_cast<double>(n);
#endif
        }
        input.swap(output);
    }
    double checksum = 0.0;
    for (double value : input) checksum += value;
    std::cout << "matrix " << std::llround(checksum * 1000000.0) << '\n';
}
