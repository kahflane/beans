#include "common.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

class Channel {
  public:
    explicit Channel(std::size_t capacity) : capacity_(capacity) {}
    void send(std::int64_t value) {
        std::unique_lock lock(mutex_);
        writable_.wait(lock, [&] { return values_.size() < capacity_; });
        values_.push_back(value);
        readable_.notify_one();
    }
    std::optional<std::int64_t> recv() {
        std::unique_lock lock(mutex_);
        readable_.wait(lock, [&] { return !values_.empty() || closed_; });
        if (values_.empty()) return std::nullopt;
        const auto value = values_.front();
        values_.pop_front();
        writable_.notify_one();
        return value;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        readable_.notify_all();
    }
  private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::deque<std::int64_t> values_;
    bool closed_ = false;
};

int main(int argc, char** argv) {
    const auto n = bench_arg(argc, argv, 0, 1000000);
    const auto seed = bench_arg(argc, argv, 1, 1);
    Channel channel(1024);
    std::int64_t sent = 0;
    std::thread producer([&] {
        for (std::int64_t i = 0; i < n; ++i)
            channel.send((i * 17 + seed) % 1000003);
        channel.close();
        sent = n;
    });
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) checksum += *channel.recv();
    producer.join();
    std::cout << "channels " << checksum << ' ' << sent << ' '
              << (channel.recv().has_value() ? "false" : "true") << '\n';
}
