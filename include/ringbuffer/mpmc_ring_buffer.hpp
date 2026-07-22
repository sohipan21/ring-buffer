#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ringbuffer/cache_line.hpp>
#include <ringbuffer/memory_order.hpp>

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
    MpmcRingBuffer() {
        // Slot i starts at seq == i: free for the producer at position i.
        // No concurrent access yet, so relaxed stores are enough.
        for (std::size_t i = 0; i < N; ++i) {
            slots_[i].seq.store(i, detail::mo_relaxed);
        }
    }

    /// Enqueues one element. Returns false if the buffer is full; nothing is
    /// claimed or written on failure.
    [[nodiscard]] bool try_push(const T& item) {
        std::size_t pos = enqueue_pos_.load(detail::mo_relaxed);
        Slot* slot;
        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->seq.load(detail::mo_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                // Free for this lap — claim it. Weak CAS: we're in a retry
                // loop anyway, and failure reloads `pos` for us.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, detail::mo_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full — nothing claimed
            } else {
                // Another producer claimed this position; catch up.
                pos = enqueue_pos_.load(detail::mo_relaxed);
            }
        }
        slot->data = item;
        slot->seq.store(pos + 1, detail::mo_release);  // publish
        return true;
    }

    /// Dequeues one element into `out`. Returns false if the buffer is
    /// empty; `out` is untouched on failure.
    [[nodiscard]] bool try_pop(T& out) {
        std::size_t pos = dequeue_pos_.load(detail::mo_relaxed);
        Slot* slot;
        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->seq.load(detail::mo_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                // Published for this lap — claim it.
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, detail::mo_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty — nothing claimed
            } else {
                // Another consumer claimed this position; catch up.
                pos = dequeue_pos_.load(detail::mo_relaxed);
            }
        }
        out = slot->data;
        slot->seq.store(pos + N, detail::mo_release);  // free for next lap
        return true;
    }

    /// Enqueues up to `count` elements from `items`. Probes the contiguous
    /// run of free slots from the current position and claims it with a
    /// single CAS; returns how many were enqueued — min(count, that run),
    /// possibly 0 (partial-batch semantics, DESIGN.md §8).
    [[nodiscard]] std::size_t try_push_batch(const T* items, std::size_t count);

    /// Dequeues up to `max_count` elements into `out`. Probes the contiguous
    /// run of ready slots and claims it with a single CAS; returns how many
    /// were dequeued — min(max_count, that run), possibly 0.
    [[nodiscard]] std::size_t try_pop_batch(T* out, std::size_t max_count);

    /// Approximate element count. Inherently racy under concurrent use —
    /// intended for metrics and debugging, never for control flow.
    [[nodiscard]] std::size_t size_approx() const {
        return enqueue_pos_.load(detail::mo_relaxed) -
               dequeue_pos_.load(detail::mo_relaxed);
    }

    /// Compile-time capacity.
    static constexpr std::size_t capacity() { return N; }

private:
    friend struct MpmcWhiteBox;  // tests read slot seqs directly

    // Each slot on its own interference-size line by default: neighbouring
    // slots get written by different threads. RINGBUFFER_PACKED_SLOTS drops the
    // padding for the density-vs-interference A/B (DESIGN.md §7); correctness is
    // the same either way.
#if defined(RINGBUFFER_PACKED_SLOTS)
    struct Slot {
#else
    struct alignas(kCacheLineSize) Slot {
#endif
        std::atomic<std::size_t> seq;
        T data;
    };

    static constexpr std::size_t kMask = N - 1;

    // The two claim counters are the hottest words in the structure; each
    // gets its own line (DESIGN.md §7).
    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(kCacheLineSize) std::array<Slot, N> slots_;

#if !defined(RINGBUFFER_PACKED_SLOTS)
    static_assert(alignof(Slot) == kCacheLineSize,
                  "padded slots must sit one per cache line");
#endif
};

// ---------------------------------------------------------------------------
// Batch operations. One CAS claims a contiguous run of k slots, amortising the
// contended position-counter RMW across the whole batch. The run is found by a
// forward scan, not a probe of the last slot: with several consumers (or
// producers) completing out of order the free region can have holes, so
// readiness is not contiguous and the last slot alone doesn't prove the rest
// (DESIGN.md §8).
// ---------------------------------------------------------------------------

template <typename T, std::size_t N>
std::size_t MpmcRingBuffer<T, N>::try_push_batch(const T* items,
                                                 std::size_t count) {
    if (count == 0) {
        return 0;
    }
    const std::size_t k0 = count < N ? count : N;  // can't claim past capacity
    std::size_t pos = enqueue_pos_.load(detail::mo_relaxed);
    std::size_t k;
    for (;;) {
        // Forward scan: longest contiguous run of slots free for this lap.
        k = 0;
        while (k < k0) {
            const std::size_t seq =
                slots_[(pos + k) & kMask].seq.load(detail::mo_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos + k);
            if (diff != 0) {
                break;  // not free (or claimed mid-scan) — run ends here
            }
            ++k;
        }
        if (k == 0) {
            return 0;  // full at the front
        }
        // Claim all k at once. Success means enqueue_pos was still pos, so no
        // other producer touched the scanned slots.
        if (enqueue_pos_.compare_exchange_weak(pos, pos + k,
                                               detail::mo_relaxed)) {
            break;
        }
        // CAS failed → pos reloaded; rescan from the new position.
    }
    for (std::size_t i = 0; i < k; ++i) {
        Slot& slot = slots_[(pos + i) & kMask];
        slot.data = items[i];
        // Publish forward so consumers can start on early items while later
        // ones are still being written.
        slot.seq.store(pos + i + 1, detail::mo_release);
    }
    return k;
}

template <typename T, std::size_t N>
std::size_t MpmcRingBuffer<T, N>::try_pop_batch(T* out, std::size_t max_count) {
    if (max_count == 0) {
        return 0;
    }
    const std::size_t k0 = max_count < N ? max_count : N;
    std::size_t pos = dequeue_pos_.load(detail::mo_relaxed);
    std::size_t k;
    for (;;) {
        // Forward scan: longest contiguous run of slots published for this lap.
        k = 0;
        while (k < k0) {
            const std::size_t seq =
                slots_[(pos + k) & kMask].seq.load(detail::mo_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos + k + 1);
            if (diff != 0) {
                break;  // not ready — run ends here
            }
            ++k;
        }
        if (k == 0) {
            return 0;  // empty at the front
        }
        if (dequeue_pos_.compare_exchange_weak(pos, pos + k,
                                               detail::mo_relaxed)) {
            break;
        }
    }
    for (std::size_t i = 0; i < k; ++i) {
        Slot& slot = slots_[(pos + i) & kMask];
        out[i] = slot.data;
        slot.seq.store(pos + i + N, detail::mo_release);  // free next lap
    }
    return k;
}

}  // namespace ringbuffer
