#include "common.h"

#include <cstdint>
#include <iostream>
#include <string>

static std::size_t rune_width(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

static std::int64_t count_chars(const std::string& text, std::size_t limit) {
    std::int64_t count = 0;
    for (std::size_t i = 0; i < limit; ++count) {
        const auto first = static_cast<unsigned char>(
#ifdef BEANS_MATCHED
            text.at(i)
#else
            text[i]
#endif
        );
        i += rune_width(first);
    }
    return count;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 100000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    const std::string unit = "héllo→🌍";
    std::string text;
    const auto repeats = n + seed % 3;
    text.reserve(unit.size() * static_cast<std::size_t>(repeats));
    for (std::int64_t i = 0; i < repeats; ++i) text += unit;
    std::int64_t bytes = 0;
    std::int64_t count = 0;
    constexpr int rounds = 50;
    for (int round = 0; round < rounds; ++round) {
        const auto limit = text.size() - static_cast<std::size_t>(round % 3) * 13;
        count += count_chars(text, limit);
        bytes += static_cast<std::int64_t>(limit);
    }
    std::cout << "utf8 " << count << ' ' << bytes << ' ' << text.size() << '\n';
}
