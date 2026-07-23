#include "common.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 5000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::int64_t> values;
    values.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        values.push_back((i * 48271 + seed) % 1000003);
    std::stable_sort(values.begin(), values.end());
    std::int64_t sum = 0;
    for (auto value : values) sum += value;
    const auto start = n / 4;
    const auto stop = std::min(start + 1000, n);
    std::int64_t middle_sum = 0;
    for (auto i = start; i < stop; ++i) {
#ifdef BEANS_MATCHED
        middle_sum += values.at(static_cast<std::size_t>(i));
#else
        middle_sum += values[static_cast<std::size_t>(i)];
#endif
    }
    const auto first = values.empty() ? -1 : values.front();
    const auto last = values.empty() ? -1 : values.back();
    std::cout << "sequences " << sum << ' ' << middle_sum << ' ' << first << ' '
              << last << ' ' << values.size() << '\n';
}
