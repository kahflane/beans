#include "common.h"

#include <cstdint>
#include <iostream>
#include <memory>

static void print_cents(std::int64_t cents) {
    if (cents < 0) { std::cout << '-'; cents = -cents; }
    std::cout << cents / 100 << '.';
    const auto rest = cents % 100;
    if (rest < 10) std::cout << '0';
    std::cout << rest;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 100000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
#ifdef BEANS_MATCHED
    auto balance = std::make_shared<std::int64_t>(seed * 100);
    for (std::int64_t i = 0; i < n; ++i) {
        const auto next = *balance + ((i + seed) % 3 == 0 ? 125 : -50);
        balance = std::make_shared<std::int64_t>(next);
    }
    std::cout << "decimal_kernel ";
    print_cents(*balance);
#else
    std::int64_t balance = seed * 100;
    for (std::int64_t i = 0; i < n; ++i)
        balance += (i + seed) % 3 == 0 ? 125 : -50;
    std::cout << "decimal_kernel ";
    print_cents(balance);
#endif
    std::cout << '\n';
}
