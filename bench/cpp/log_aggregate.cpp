#include "common.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef BEANS_MATCHED
class OrderedMap {
  public:
    void reserve(std::size_t n) { entries_.reserve(n); index_.reserve(n); }
    std::int64_t get(const std::string& key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? 0 : entries_[found->second].second;
    }
    void set(const std::string& key, std::int64_t value) {
        const auto found = index_.find(key);
        if (found != index_.end()) { entries_[found->second].second = value; return; }
        index_[key] = entries_.size();
        entries_.push_back({key, value});
    }
    std::size_t size() const { return entries_.size(); }
  private:
    std::vector<std::pair<std::string, std::int64_t>> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};
#endif

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<std::string>> rows;
#else
    std::vector<std::string> rows;
#endif
    rows.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto level = (i + seed) % 7 == 0 ? "error" : "info";
        auto row = "user" + std::to_string((i * 17 + seed) % 4096) + ',' + level + ',' +
                   std::to_string((i * 31 + seed) % 1000);
#ifdef BEANS_MATCHED
        rows.push_back(std::make_shared<std::string>(std::move(row)));
#else
        rows.push_back(std::move(row));
#endif
    }
#ifdef BEANS_MATCHED
    OrderedMap users;
#else
    std::unordered_map<std::string, std::int64_t> users;
#endif
    users.reserve(4096);
    std::int64_t total = 0, errors = 0;
    for (const auto& stored : rows) {
#ifdef BEANS_MATCHED
        const std::string& row = *stored;
#else
        const std::string& row = stored;
#endif
        const auto first = row.find(',');
        const auto second = row.find(',', first + 1);
        const auto user = row.substr(0, first);
        const auto level = row.substr(first + 1, second - first - 1);
        const auto value_text = std::string_view(row).substr(second + 1);
#ifdef BEANS_MATCHED
        users.set(user, users.get(user) + 1);
#else
        ++users[user];
#endif
        if (level == "error") ++errors;
        std::int64_t value = 0;
        std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
        total += value;
    }
    std::cout << "log_aggregate " << users.size() << ' ' << errors << ' ' << total << '\n';
}
