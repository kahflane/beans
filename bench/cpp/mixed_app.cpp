#include "common.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Event { std::int64_t user; std::string label; std::int64_t value; };

#ifdef BEANS_MATCHED
class OrderedMap {
  public:
    void reserve(std::size_t n) { entries_.reserve(n); index_.reserve(n); }
    std::int64_t get(std::int64_t key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? 0 : entries_[found->second].second;
    }
    void set(std::int64_t key, std::int64_t value) {
        const auto found = index_.find(key);
        if (found != index_.end()) { entries_[found->second].second = value; return; }
        index_[key] = entries_.size(); entries_.push_back({key, value});
    }
    std::size_t size() const { return entries_.size(); }
  private:
    std::vector<std::pair<std::int64_t, std::int64_t>> entries_;
    std::unordered_map<std::int64_t, std::size_t> index_;
};
#endif

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1500000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        Event event{(i * 17 + seed) % 8192,
                    "event" + std::to_string((i + seed) % 32),
                    (i * 31 + seed) % 1000};
        events.push_back(std::move(event));
    }
#ifdef BEANS_MATCHED
    OrderedMap totals;
#else
    std::unordered_map<std::int64_t, std::int64_t> totals;
#endif
    totals.reserve(8192);
    std::vector<std::int64_t> selected;
    selected.reserve(static_cast<std::size_t>(n / 7 + 1));
    for (const Event& event : events) {
#ifdef BEANS_MATCHED
        totals.set(event.user, totals.get(event.user) + event.value + event.label.size());
#else
        totals[event.user] += event.value + static_cast<std::int64_t>(event.label.size());
#endif
        if ((event.user + seed) % 7 == 0) selected.push_back(event.value);
    }
    std::stable_sort(selected.begin(), selected.end());
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < 8192; ++i) {
#ifdef BEANS_MATCHED
        checksum += totals.get(i);
#else
        const auto found = totals.find(i);
        if (found != totals.end()) checksum += found->second;
#endif
    }
    std::cout << "mixed_app " << checksum << ' ' << totals.size() << ' '
              << selected.size() << ' ' << selected[selected.size() / 2] << '\n';
}
