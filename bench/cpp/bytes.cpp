#include "common.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

static void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) byte |= 0x80;
        out.push_back(byte);
    } while (value);
}

static std::uint64_t read_varint(const std::vector<std::uint8_t>& data,
                                 std::size_t& pos) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    while (true) {
#ifdef BEANS_MATCHED
        const auto byte = data.at(pos++);
#else
        const auto byte = data[pos++];
#endif
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return value;
        shift += 7;
    }
}

static std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xffffffffu;
    for (auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 500000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    std::vector<std::uint8_t> data;
    data.reserve(static_cast<std::size_t>(n) * 3);
    for (std::int64_t i = 0; i < n; ++i)
        append_varint(data, static_cast<std::uint64_t>((i * 48271 + seed) % 1000003));
    std::size_t pos = 0;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i)
        checksum += static_cast<std::int64_t>(read_varint(data, pos));
    std::cout << "bytes " << checksum << ' ' << data.size() << ' ' << crc32(data) << '\n';
}
