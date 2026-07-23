#include "common.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

class OrderedMap {
  public:
    void reserve(std::size_t n) { entries_.reserve(n); index_.reserve(n); }
    void set(std::int64_t key, std::int64_t value) {
        const auto found = index_.find(key);
        if (found != index_.end()) { entries_[found->second].value = value; return; }
        index_[key] = entries_.size();
        entries_.push_back({key, value, false});
    }
    void remove(std::int64_t key) {
        const auto found = index_.find(key);
        if (found == index_.end()) return;
        entries_[found->second].dead = true;
        index_.erase(found);
    }
    std::vector<std::int64_t> keys() const {
        std::vector<std::int64_t> result;
        result.reserve(index_.size());
        for (const auto& entry : entries_)
            if (!entry.dead) result.push_back(entry.key);
        return result;
    }
    std::size_t size() const { return index_.size(); }
  private:
    struct Entry { std::int64_t key; std::int64_t value; bool dead; };
    std::vector<Entry> entries_;
    std::unordered_map<std::int64_t, std::size_t> index_;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 2000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    OrderedMap values;
    values.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto key = (i * 48271 + seed) % n;
        values.set(key, i + seed);
    }
    for (std::int64_t i = 0; i < n; ++i)
        if (i % 5 == 0) values.remove((i * 48271 + seed) % n);
    for (std::int64_t i = n - 1; i >= 0; --i)
        if (i % 5 == 0) values.set((i * 48271 + seed) % n, i + seed + 1);
    const auto keys = values.keys();
    std::int64_t checksum = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
#ifdef BEANS_MATCHED
        checksum += keys.at(i) * (static_cast<std::int64_t>(i % 97) + 1);
#else
        checksum += keys[i] * (static_cast<std::int64_t>(i % 97) + 1);
#endif
    }
    std::cout << "ordered_maps " << checksum << ' ' << values.size() << ' '
              << keys.front() << ' ' << keys.back() << '\n';
}
