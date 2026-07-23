#include "common.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

struct Counter { std::int64_t value = 0; };

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 5000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    Counter counter{seed};
    std::mutex mutex;
    const auto q = n / 4;
    auto bump = [&](std::int64_t count) {
        for (std::int64_t i = 0; i < count; ++i) {
#ifdef BEANS_MATCHED
            std::function<void(Counter&)> body = [](Counter& value) { ++value.value; };
            std::lock_guard lock(mutex);
            body(counter);
#else
            std::lock_guard lock(mutex);
            ++counter.value;
#endif
        }
        return count;
    };
    std::int64_t done[4]{};
    std::thread t0([&] { done[0] = bump(q); });
    std::thread t1([&] { done[1] = bump(q); });
    std::thread t2([&] { done[2] = bump(q); });
    std::thread t3([&] { done[3] = bump(n - q * 3); });
    t0.join(); t1.join(); t2.join(); t3.join();
    std::cout << "mutex_contention " << counter.value << ' '
              << done[0] + done[1] + done[2] + done[3] << '\n';
}
