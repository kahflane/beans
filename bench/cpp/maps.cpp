#include "common.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

template <class K, class V>
class OrderedMap {
  public:
    void reserve(std::size_t capacity) {
        entries_.reserve(capacity);
        index_.reserve(capacity);
    }

    void set(K key, V value) {
        auto found = index_.find(key);
        if (found != index_.end()) {
            entries_[found->second].second = std::move(value);
            return;
        }
        index_.emplace(key, entries_.size());
        entries_.emplace_back(std::move(key), std::move(value));
    }

    const V& get(const K& key) const { return entries_[index_.at(key)].second; }
    bool contains(const K& key) const { return index_.find(key) != index_.end(); }
    std::size_t size() const { return entries_.size(); }

  private:
    std::vector<std::pair<K, V>> entries_;
    std::unordered_map<K, std::size_t> index_;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 400000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    OrderedMap<std::int64_t, std::int64_t> map;
    map.reserve(static_cast<std::size_t>(n));
#else
    std::unordered_map<std::int64_t, std::int64_t> map;
    map.reserve(static_cast<std::size_t>(n));
#endif
    for (std::int64_t i = 0; i < n; ++i) {
#ifdef BEANS_MATCHED
        map.set(i + seed, i * 2 + seed);
#else
        map[i + seed] = i * 2 + seed;
#endif
    }
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
#ifdef BEANS_MATCHED
        sum += map.get(i + seed);
#else
        sum += map.at(i + seed);
#endif
    }
    std::int64_t hits = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (map.contains(i + seed)) ++hits;
        if (map.contains(i + n + seed)) ++hits;
    }

    const auto string_n = n / 5;
#ifdef BEANS_MATCHED
    OrderedMap<std::string, std::int64_t> strings;
    strings.reserve(static_cast<std::size_t>(string_n));
#else
    std::unordered_map<std::string, std::int64_t> strings;
    strings.reserve(static_cast<std::size_t>(string_n));
#endif
    for (std::int64_t i = 0; i < string_n; ++i) {
        auto key = "key" + std::to_string(seed) + "_" + std::to_string(i);
#ifdef BEANS_MATCHED
        strings.set(std::move(key), i + seed);
#else
        strings.emplace(std::move(key), i + seed);
#endif
    }
    std::int64_t string_sum = 0;
    for (std::int64_t i = 0; i < string_n; ++i) {
        auto key = "key" + std::to_string(seed) + "_" + std::to_string(i);
#ifdef BEANS_MATCHED
        string_sum += strings.get(key);
#else
        string_sum += strings.at(key);
#endif
    }
    std::cout << "maps " << sum << ' ' << hits << ' ' << string_sum << ' '
              << map.size() << ' ' << strings.size() << '\n';
}
