#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

struct P {
    std::int64_t a;
    std::int64_t b;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 5000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t sum = 0;
#ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<P>> keep;
    keep.reserve(static_cast<std::size_t>(n / 1000 + 1));
    for (std::int64_t i = 0; i < n; ++i) {
        auto p = std::make_shared<P>(P{i + seed, i + seed + 1});
        auto q = p;
        sum += q->a + p->b;
        if (i % 1000 == 0) keep.push_back(std::move(p));
    }
#else
    std::vector<std::unique_ptr<P>> keep;
    keep.reserve(static_cast<std::size_t>(n / 1000 + 1));
    for (std::int64_t i = 0; i < n; ++i) {
        auto p = std::make_unique<P>(P{i + seed, i + seed + 1});
        P* q = p.get();
        sum += q->a + p->b;
        if (i % 1000 == 0) keep.push_back(std::move(p));
    }
#endif
    std::cout << "sum " << sum << " kept " << keep.size() << '\n';
}
