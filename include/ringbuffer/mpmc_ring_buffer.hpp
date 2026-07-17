#pragma once

#include <cstddef>
#include <type_traits>

namespace ringbuffer {

/// Bounded multi-producer / multi-consumer FIFO ring buffer (Vyukov-style
/// sequence-number design).
///
/// Lockless / non-blocking: no operation takes a lock; try_push / try_pop
/// return false instead of waiting. Not formally lock-free — see DESIGN.md §5
/// for the precise progress guarantees.
///
/// v1 constraints, both enforced at compile time (DESIGN.md §9):
///   - T must be trivially copyable
///   - N must be a power of two, at least 2
template <typename T, std::size_t N>
class MpmcRingBuffer {
    static_assert(N >= 2 && (N & (N - 1)) == 0,
                  "capacity must be a power of two, at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "v1 supports trivially copyable element types only");

public:
    /// Enqueues one element. Returns false if the buffer is full; nothing is
    /// claimed or written on failure.
    [[nodiscard]] bool try_push(const T& item);

    /// Dequeues one element into `out`. Returns false if the buffer is empty;
    /// `out` is untouched on failure.
    [[nodiscard]] bool try_pop(T& out);

    /// Enqueues up to `count` elements from `items`. Returns the number
    /// actually enqueued — possibly fewer than `count`, possibly 0
    /// (partial-batch semantics, DESIGN.md §8).
    [[nodiscard]] std::size_t try_push_batch(const T* items, std::size_t count);

    /// Dequeues up to `max_count` elements into `out`. Returns the number
    /// actually dequeued — possibly 0.
    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count);

    /// Approximate element count. Inherently racy under concurrent use —
    /// intended for metrics and debugging, never for control flow.
    [[nodiscard]] std::size_t size_approx() const;

    /// Compile-time capacity.
    static constexpr std::size_t capacity() { return N; }
};

// ---------------------------------------------------------------------------
// Stub bodies — the protocol from DESIGN.md §3 lands next.
// ---------------------------------------------------------------------------

template <typename T, std::size_t N>
bool MpmcRingBuffer<T, N>::try_push(const T& /*item*/) {
    return false;  // TODO: CAS claim loop
}

template <typename T, std::size_t N>
bool MpmcRingBuffer<T, N>::try_pop(T& /*out*/) {
    return false;  // TODO: CAS claim loop
}

template <typename T, std::size_t N>
std::size_t MpmcRingBuffer<T, N>::try_push_batch(const T* /*items*/,
                                                 std::size_t /*count*/) {
    return 0;  // TODO: single-CAS batch claim
}

template <typename T, std::size_t N>
std::size_t MpmcRingBuffer<T, N>::try_pop_batch(T* /*out*/,
                                                std::size_t /*max_count*/) {
    return 0;  // TODO: single-CAS batch claim
}

template <typename T, std::size_t N>
std::size_t MpmcRingBuffer<T, N>::size_approx() const {
    return 0;  // TODO: enqueue/dequeue position difference
}

}  // namespace ringbuffer
