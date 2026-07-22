#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

namespace ringbuffer::bench {

// The obvious baseline: a bounded FIFO guarded by one mutex. Same API as the
// ring buffers, so the throughput/latency drivers are generic over queue type.
// This is what most people reach for first; the point of the project is to show
// what the lockless design buys over it.
template <typename T, std::size_t N>
class MutexQueue {
public:
    bool try_push(const T& item) {
        std::lock_guard<std::mutex> g(mutex_);
        if (queue_.size() >= N) {
            return false;
        }
        queue_.push_back(item);
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

    std::size_t try_push_batch(const T* items, std::size_t count) {
        std::lock_guard<std::mutex> g(mutex_);
        const std::size_t room = N - queue_.size();
        const std::size_t n = count < room ? count : room;
        for (std::size_t i = 0; i < n; ++i) {
            queue_.push_back(items[i]);
        }
        return n;
    }

    std::size_t try_pop_batch(T* out, std::size_t max_count) {
        std::lock_guard<std::mutex> g(mutex_);
        const std::size_t n = max_count < queue_.size() ? max_count : queue_.size();
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = queue_.front();
            queue_.pop_front();
        }
        return n;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    std::mutex mutex_;
    std::deque<T> queue_;
};

}  // namespace ringbuffer::bench
