#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>

#ifdef BEANS_MATCHED
using NodePtr = std::shared_ptr<struct Node>;
#else
using NodePtr = std::unique_ptr<struct Node>;
#endif

struct Node {
    NodePtr left;
    NodePtr right;
    std::int64_t value = 0;
};

static NodePtr build(int depth, std::int64_t seed) {
#ifdef BEANS_MATCHED
    auto node = std::make_shared<Node>();
#else
    auto node = std::make_unique<Node>();
#endif
    node->value = depth + seed;
    if (depth > 0) {
        node->left = build(depth - 1, seed);
        node->right = build(depth - 1, seed);
    }
    return node;
}

static std::int64_t count(const Node& node) {
    std::int64_t total = 1 + node.value;
    if (node.left) total += count(*node.left);
    if (node.right) total += count(*node.right);
    return total;
}

int main(int argc, char** argv) {
    const int max_depth = static_cast<int>(bench_arg(argc, argv, 0, 16));
    const auto seed = bench_arg(argc, argv, 1, 1);
    auto long_lived = build(max_depth, seed);
    std::int64_t total = 0;
    for (int depth = 4; depth <= max_depth; depth += 2) {
        std::int64_t iterations = 1;
        for (int k = max_depth - depth + 4; k > 0; --k) iterations *= 2;
        for (std::int64_t i = 0; i < iterations; ++i)
            total += count(*build(depth, seed));
    }
    std::cout << "trees " << total << " long " << count(*long_lived) << '\n';
}
