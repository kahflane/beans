#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>

struct Node { std::int64_t value; std::shared_ptr<Node> next; };

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 2000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        auto left = std::make_shared<Node>(Node{i + seed, {}});
        auto right = std::make_shared<Node>(Node{i + seed + 1, {}});
        left->next = right;
        right->next = left;
        checksum += left->value + right->value;
        left->next.reset();
        right->next.reset();
    }
    std::cout << "cycles " << checksum << '\n';
}
