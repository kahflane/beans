#include "common.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 8000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::int64_t> values;
    values.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) values.push_back((i * 31 + seed) % 1000003);
    constexpr std::int64_t width = 1024;
    const auto rounds = n / 20;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < rounds; ++i) {
        const auto start = (i * 97 + seed) % (n - width);
        std::vector<std::int64_t> part(values.begin() + start,
                                       values.begin() + start + width);
        bench_escape(part.data());
#ifdef BEANS_MATCHED
        checksum += part.at(0) + part.at(width - 1) + part.size();
#else
        checksum += part[0] + part[width - 1] + part.size();
#endif
    }
    std::cout << "slices " << checksum << ' ' << rounds << '\n';
}
