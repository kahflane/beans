#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

static std::string replace_all(std::string_view source, std::string_view from,
                               std::string_view to) {
    std::string result;
    result.reserve(source.size());
    std::size_t at = 0;
    while (at < source.size()) {
        const auto found = source.find(from, at);
        if (found == std::string_view::npos) {
            result.append(source.substr(at));
            break;
        }
        result.append(source.substr(at, found - at));
        result.append(to);
        at = found + from.size();
    }
    return result;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 2000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    #ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<std::string>> parts;
    #else
    std::vector<std::string> parts;
    #endif
    parts.reserve(static_cast<std::size_t>(n));
    std::size_t bytes = n > 0 ? static_cast<std::size_t>(n - 1) : 0;
    for (std::int64_t i = 0; i < n; ++i) {
        auto part = "item" + std::to_string(seed) + "_" + std::to_string(i);
        bytes += part.size();
        #ifdef BEANS_MATCHED
        parts.push_back(std::make_shared<std::string>(std::move(part)));
        #else
        parts.push_back(std::move(part));
        #endif
    }
    std::string joined;
    joined.reserve(bytes);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back(',');
        #ifdef BEANS_MATCHED
        joined += *parts[i];
        #else
        joined += parts[i];
        #endif
    }
    std::string upper = joined;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    std::int64_t hits = 0;
    std::size_t count = 0;
    #ifdef BEANS_MATCHED
    std::vector<std::shared_ptr<std::string>> back;
    back.reserve(parts.size());
    std::size_t start = 0;
    while (start < joined.size()) {
        const auto comma = joined.find(',', start);
        const auto end = comma == std::string::npos ? joined.size() : comma;
        back.push_back(std::make_shared<std::string>(joined.substr(start, end - start)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    count = back.size();
    for (const auto& part : back)
        if (part->find("999") != std::string::npos) ++hits;
    #else
    count = joined.empty() ? 0 : 1;
    std::size_t start = 0;
    while (start < joined.size()) {
        const auto comma = joined.find(',', start);
        const auto end = comma == std::string::npos ? joined.size() : comma;
        if (std::string_view(joined).substr(start, end - start).find("999") !=
            std::string_view::npos)
            ++hits;
        if (comma == std::string::npos) break;
        ++count;
        start = comma + 1;
    }
    #endif
    auto swapped = replace_all(joined, "item", "row");
    std::cout << "strings " << joined.size() << ' ' << upper.size() << ' '
              << count << ' ' << hits << ' ' << swapped.size() << '\n';
}
