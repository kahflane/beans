#include "common.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 10000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::atomic<std::int64_t> counter(seed);
    const auto q = n / 4;
    auto add = [&](std::int64_t count) {
        for (std::int64_t i = 0; i < count; ++i)
            counter.fetch_add(1, std::memory_order_seq_cst);
        return count;
    };
    std::int64_t done[4]{};
    std::thread t0([&] { done[0] = add(q); });
    std::thread t1([&] { done[1] = add(q); });
    std::thread t2([&] { done[2] = add(q); });
    std::thread t3([&] { done[3] = add(n - q * 3); });
    t0.join(); t1.join(); t2.join(); t3.join();
    std::cout << "atomic_contention " << counter.load() << ' '
              << done[0] + done[1] + done[2] + done[3] << '\n';
}
