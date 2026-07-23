#include "common.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

struct Record { std::int64_t key; std::int64_t order; };

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 2000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<Record>> records;
    records.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        records.push_back(std::make_shared<Record>(Record{(i * 48271 + seed) % 4096, i}));
    std::stable_sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left->key < right->key;
    });
    std::int64_t checksum = 0;
    for (const auto& record : records) checksum += record->key * 17 + record->order;
    std::cout << "sort_objects " << checksum << ' ' << records.front()->key << ' '
              << records.back()->key << '\n';
#else
    // Beans `class` values have object identity and one allocation each.
    // unique_ptr is the tuned ownership match; an inline vector<Record>
    // measures a different Beans `struct` workload.
    std::vector<std::unique_ptr<Record>> records;
    records.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        records.push_back(
            std::make_unique<Record>(Record{(i * 48271 + seed) % 4096, i}));
    std::stable_sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left->key < right->key;
    });
    std::int64_t checksum = 0;
    for (const auto& record : records) checksum += record->key * 17 + record->order;
    std::cout << "sort_objects " << checksum << ' ' << records.front()->key << ' '
              << records.back()->key << '\n';
#endif
}
