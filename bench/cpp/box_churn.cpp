#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 10000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
    std::vector<std::unique_ptr<std::int64_t>> batch;
    batch.reserve(1024);
    for (std::int64_t i = 0; i < n; ++i) {
        auto value = std::make_unique<std::int64_t>(i + seed);
        sum += *value;
        *value += 3;
        sum += *value;
        batch.push_back(std::move(value));
        if (batch.size() == 1024) batch.clear();
    }
    std::cout << "sum " << sum << '\n';
}
