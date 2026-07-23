#include "common.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::int64_t> values;
    values.reserve(4096);
    for (std::int64_t i = 0; i < 2048; ++i) values.push_back(i + seed);
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto at = (i * 48271 + seed) % static_cast<std::int64_t>(values.size());
#ifdef BEANS_MATCHED
        checksum += values.at(static_cast<std::size_t>(at));
#else
        checksum += values[static_cast<std::size_t>(at)];
#endif
        values.erase(values.begin() + at);
        const auto put = (at * 17 + seed) % static_cast<std::int64_t>(values.size() + 1);
        values.insert(values.begin() + put, i + seed);
    }
    std::cout << "sequence_churn " << checksum << ' ' << values.size() << ' '
              << values.front() << '\n';
}
