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
        if (found != index_.end()) { entries_[found->second].second = value; return; }
        index_[key] = entries_.size();
        entries_.push_back({key, value});
    }
    const std::int64_t* get(std::int64_t key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? nullptr : &entries_[found->second].second;
    }
    std::size_t size() const { return entries_.size(); }
  private:
    std::vector<std::pair<std::int64_t, std::int64_t>> entries_;
    std::unordered_map<std::int64_t, std::size_t> index_;
};
#endif

static void append_i64(std::vector<std::uint8_t>& out, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(bits >> shift));
}

static std::int64_t get_i64(const std::vector<std::uint8_t>& data, std::size_t pos) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
#ifdef BEANS_MATCHED
        value |= static_cast<std::uint64_t>(data.at(pos++)) << shift;
#else
        value |= static_cast<std::uint64_t>(data[pos++]) << shift;
#endif
    }
    return static_cast<std::int64_t>(value);
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 3000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    const auto key_count = n / 4 + 1;
    std::vector<std::uint8_t> log;
    log.reserve(static_cast<std::size_t>(n) * 24);
#ifdef BEANS_MATCHED
    OrderedMap latest;
#else
    std::unordered_map<std::int64_t, std::int64_t> latest;
#endif
    latest.reserve(static_cast<std::size_t>(key_count));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto key = (i * 48271 + seed) % key_count;
        const auto value = i * 17 + seed;
#ifdef BEANS_MATCHED
        latest.set(key, static_cast<std::int64_t>(log.size()));
#else
        latest[key] = static_cast<std::int64_t>(log.size());
#endif
        append_i64(log, key); append_i64(log, value); append_i64(log, i);
    }
#ifdef BEANS_MATCHED
    OrderedMap rebuilt;
#else
    std::unordered_map<std::int64_t, std::int64_t> rebuilt;
#endif
    rebuilt.reserve(static_cast<std::size_t>(key_count));
    for (std::size_t pos = 0; pos < log.size(); pos += 24) {
#ifdef BEANS_MATCHED
        rebuilt.set(get_i64(log, pos), static_cast<std::int64_t>(pos));
#else
        rebuilt[get_i64(log, pos)] = static_cast<std::int64_t>(pos);
#endif
    }
    std::vector<std::uint8_t> compact;
    compact.reserve(static_cast<std::size_t>(key_count) * 24);
    std::int64_t checksum = 0;
    for (std::int64_t key = 0; key < key_count; ++key) {
#ifdef BEANS_MATCHED
        const auto* found = rebuilt.get(key);
        if (!found) continue;
        const auto at = *found;
#else
        const auto found = rebuilt.find(key);
        if (found == rebuilt.end()) continue;
        const auto at = found->second;
#endif
        checksum += get_i64(log, static_cast<std::size_t>(at + 8));
        compact.insert(compact.end(), log.begin() + at, log.begin() + at + 24);
    }
    std::cout << "kv_store " << checksum << ' ' << latest.size() << ' '
              << rebuilt.size() << ' ' << log.size() << ' ' << compact.size() << '\n';
}
