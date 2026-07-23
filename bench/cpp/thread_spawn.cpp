#include "common.h"

#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 10000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t value = 0;
        std::thread worker([&, i] { value = (i * 17 + seed) % 1000003; });
        worker.join();
        checksum += value;
    }
    std::cout << "thread_spawn " << checksum << '\n';
}
