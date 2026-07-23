#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>

#ifdef BEANS_MATCHED
struct Node { std::int64_t value; std::shared_ptr<Node> next; };
#else
struct Node { std::int64_t value; std::unique_ptr<Node> next; };
#endif

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 4000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    auto head = std::make_shared<Node>(Node{seed, {}});
    for (std::int64_t i = 1; i < n; ++i) {
        auto next = std::make_shared<Node>(Node{i + seed, head});
        head = std::move(next);
    }
#else
    auto head = std::make_unique<Node>(Node{seed, {}});
    for (std::int64_t i = 1; i < n; ++i)
        head = std::make_unique<Node>(Node{i + seed, std::move(head)});
#endif
    const auto last = head->value;
    while (head) head = std::move(head->next);
    std::cout << "deep_teardown " << last << " -1\n";
}
