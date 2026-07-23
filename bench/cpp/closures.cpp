#include "common.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 400000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    const std::function<std::int64_t(std::int64_t)> plain =
        [](std::int64_t value) { return value * 3 + 1; };
    const auto offset = std::make_shared<std::int64_t>(seed % 97);
    const std::function<std::int64_t(std::int64_t)> captured =
        [offset](std::int64_t value) { return value + *offset; };
#else
    const auto plain = [](std::int64_t value) { return value * 3 + 1; };
    const auto offset = seed % 97;
    const auto captured = [offset](std::int64_t value) { return value + offset; };
#endif
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (i % 2 == 0) sum += plain(i % 1009);
        else sum += captured(i % 1009);
    }
    std::cout << "closures " << sum << '\n';
}
