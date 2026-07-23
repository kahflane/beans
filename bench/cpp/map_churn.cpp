#include "common.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef BEANS_MATCHED
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
    const std::int64_t* get(std::int64_t key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? nullptr : &entries_[found->second].value;
    }
    std::size_t size() const { return index_.size(); }
  private:
    struct Entry { std::int64_t key; std::int64_t value; bool dead; };
    std::vector<Entry> entries_;
    std::unordered_map<std::int64_t, std::size_t> index_;
};
#endif

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 10000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    const auto keys = n / 4 + 1;
#ifdef BEANS_MATCHED
    OrderedMap values;
#else
    std::unordered_map<std::int64_t, std::int64_t> values;
#endif
    values.reserve(static_cast<std::size_t>(keys));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto key = (i * 17 + seed) % keys;
#ifdef BEANS_MATCHED
        values.set(key, i + seed);
        if (i % 5 == 0) values.remove((key + 3) % keys);
#else
        values[key] = i + seed;
        if (i % 5 == 0) values.erase((key + 3) % keys);
#endif
    }
    std::int64_t checksum = 0;
    std::int64_t hits = 0;
    for (std::int64_t i = 0; i < keys; ++i) {
#ifdef BEANS_MATCHED
        if (const auto* value = values.get(i)) { checksum += *value; ++hits; }
#else
        const auto found = values.find(i);
        if (found != values.end()) { checksum += found->second; ++hits; }
#endif
    }
    std::cout << "map_churn " << checksum << ' ' << hits << ' ' << values.size() << '\n';
}
