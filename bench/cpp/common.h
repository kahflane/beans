#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

inline std::int64_t bench_arg(int argc, char** argv, int at,
                              std::int64_t fallback) {
    if (argc <= at + 1) return fallback;
    std::int64_t value = 0;
    std::string_view text(argv[at + 1]);
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
               ? value
               : fallback;
}

// Make constructed data observable to the optimizer without adding work to
// the timed result. This is used only where the benchmark contract requires a
// copy/allocation that whole-program LTO could otherwise prove unused.
inline void bench_escape(const void* pointer) {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "g"(pointer) : "memory");
#else
    static void const* volatile sink;
    sink = pointer;
#endif
}
