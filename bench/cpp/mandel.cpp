#include "common.h"

#include <iostream>

int main(int argc, char** argv) {
    const int size = static_cast<int>(bench_arg(argc, argv, 0, 1800));
    const auto seed = bench_arg(argc, argv, 1, 1);
    const int max_iter = 80 + static_cast<int>(seed % 41);
    int inside = 0;
    for (int y = 0; y < size; ++y) {
        const double ci = static_cast<double>(y) * 2.0 / size - 1.0;
        for (int x = 0; x < size; ++x) {
            const double cr = static_cast<double>(x) * 3.0 / size - 2.0;
            double zr = 0.0;
            double zi = 0.0;
            int i = 0;
            for (; i < max_iter; ++i) {
                const double next = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = next;
                if (zr * zr + zi * zi > 4.0) break;
            }
            if (i == max_iter) ++inside;
        }
    }
    std::cout << "mandel " << inside << '\n';
}
