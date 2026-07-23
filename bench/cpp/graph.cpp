#include "common.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class OrderedSet {
  public:
    void reserve(std::size_t capacity) {
        values_.reserve(capacity);
        index_.reserve(capacity);
    }

    bool insert(std::int64_t value) {
        if (index_.contains(value)) return false;
        index_.emplace(value, values_.size());
        values_.push_back(value);
        return true;
    }
    std::size_t size() const { return values_.size(); }

  private:
    std::vector<std::int64_t> values_;
    std::unordered_map<std::int64_t, std::size_t> index_;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 2000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::int64_t> work = {0};
    work.reserve(static_cast<std::size_t>(n));
#ifndef BEANS_MATCHED
    std::unordered_set<std::int64_t> seen;
    seen.reserve(static_cast<std::size_t>(n));
#else
    OrderedSet seen;
    seen.reserve(static_cast<std::size_t>(n));
#endif
    std::int64_t checksum = 0;
    std::int64_t edges = 0;
    while (!work.empty()) {
        const auto node = work.back();
        work.pop_back();
#ifdef BEANS_MATCHED
        const bool fresh = seen.insert(node);
#else
        const bool fresh = seen.insert(node).second;
#endif
        if (!fresh) continue;
        checksum += node;
        if (node + 1 < n) {
            work.push_back(node + 1);
            ++edges;
        }
        work.push_back((node * 17 + seed) % n);
        work.push_back((node * 31 + seed + 7) % n);
        edges += 2;
    }
    std::cout << "graph " << seen.size() << ' ' << edges << ' ' << checksum << '\n';
}
