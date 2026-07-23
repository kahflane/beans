#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>

#ifdef BEANS_MATCHED
struct Node {
    std::int64_t value;
    std::shared_ptr<Node> next;
};
#else
struct Node {
    std::int64_t value;
    std::unique_ptr<Node> next;
};
#endif

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    std::shared_ptr<Node> head;
    for (std::int64_t i = 0; i < n; ++i)
        head = std::make_shared<Node>(Node{i + seed, head});
    auto cursor = head;
#else
    std::unique_ptr<Node> head;
    for (std::int64_t i = 0; i < n; ++i)
        head = std::make_unique<Node>(Node{i + seed, std::move(head)});
    const Node* cursor = head.get();
#endif
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        checksum += cursor->value;
#ifdef BEANS_MATCHED
        cursor = cursor->next;
#else
        cursor = cursor->next.get();
#endif
    }
    std::cout << "option_chain " << checksum << " true\n";
#ifdef BEANS_MATCHED
    while (head) {
        auto next = std::move(head->next);
        head.reset();
        head = std::move(next);
    }
#else
    while (head) head = std::move(head->next);
#endif
}
