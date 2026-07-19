#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include <ringbuffer/cache_line.hpp>

namespace ringbuffer {

/// Bounded single-producer / single-consumer FIFO ring buffer.
///
/// Exactly one thread may push and exactly one thread may pop; nothing else
/// is synchronised. Non-blocking: try_push / try_pop return false instead of
/// waiting. Design rationale and memory-order argument: DESIGN.md §10.
///
/// v1 constraints, both enforced at compile time (DESIGN.md §9):
///   - T must be trivially copyable
///   - N must be a power of two, at least 2
template <typename T, std::size_t N>
class SpscRingBuffer {
    static_assert(N >= 2 && (N & (N - 1)) == 0,
                  "capacity must be a power of two, at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "v1 supports trivially copyable element types only");

public:
    /// Enqueues one element. Returns false if the buffer is full.
    /// Producer thread only.
    [[nodiscard]] bool try_push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - cached_tail_ == N) {
            // Looks full through the cached view — refresh from the consumer.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head - cached_tail_ == N) {
                return false;  // genuinely full
            }
        }
        data_[head & kMask] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Dequeues one element into `out`. Returns false if the buffer is
    /// empty; `out` is untouched on failure. Consumer thread only.
    [[nodiscard]] bool try_pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (cached_head_ == tail) {
            // Looks empty through the cached view — refresh from the producer.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (cached_head_ == tail) {
                return false;  // genuinely empty
            }
        }
        out = data_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Approximate element count. Exact only while neither thread is
    /// mutating; intended for metrics and debugging.
    [[nodiscard]] std::size_t size_approx() const {
        return head_.load(std::memory_order_relaxed) -
               tail_.load(std::memory_order_relaxed);
    }

    /// Compile-time capacity.
    static constexpr std::size_t capacity() { return N; }

private:
    static constexpr std::size_t kMask = N - 1;

    // Fields are grouped by owning thread, one interference-size line per
    // group: each thread's hot path touches only its own line until the
    // cached view forces a refresh.

    // Producer's line: its own index plus its cached copy of the consumer's.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};

    // Consumer's line, mirrored.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};

    // Both threads touch the slots; alignment keeps them off the index lines.
    alignas(kCacheLineSize) std::array<T, N> data_{};
};

}  // namespace ringbuffer
