#include "common.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 20000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::int64_t> arena;
    arena.reserve(4096);
    std::int64_t state = seed + 1;
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        state = (state * 48271 + i + 1) % 2147483647;
        const auto handle = static_cast<std::int64_t>(arena.size());
        arena.push_back(state);
        const auto pick = (state + i) % (handle + 1);
#ifdef BEANS_MATCHED
        state = (state + arena.at(static_cast<std::size_t>(pick))) % 2147483647;
#else
        state = (state + arena[static_cast<std::size_t>(pick)]) % 2147483647;
#endif
        sum += state;
        if (arena.size() == 4096) arena.clear();
    }
    std::cout << "sum " << sum << " left " << arena.size() << '\n';
}
