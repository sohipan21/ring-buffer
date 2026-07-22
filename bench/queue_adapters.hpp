#pragma once

#include <cstddef>

// Adapters that give a third-party queue the same try_push / try_pop (+ batch)
// shape as the ring buffers, so the benchmark drivers stay generic. Only
// compiled when the baseline was fetched (RINGBUFFER_FETCH_BASELINES → the
// RINGBUFFER_HAVE_MOODYCAMEL define).

#if defined(RINGBUFFER_HAVE_MOODYCAMEL)

#include <concurrentqueue.h>

namespace ringbuffer::bench {

// moodycamel::ConcurrentQueue is UNBOUNDED — try_push never reports full. That's
// a real semantic difference from this bounded queue (it can't apply
// backpressure), noted in the results rather than papered over. The N is only a
// starting-size hint.
template <typename T, std::size_t N>
class MoodycamelQueue {
public:
    MoodycamelQueue() : queue_(N) {}

    bool try_push(const T& item) { return queue_.enqueue(item); }
    bool try_pop(T& out) { return queue_.try_dequeue(out); }

    std::size_t try_push_batch(const T* items, std::size_t count) {
        return queue_.enqueue_bulk(items, count) ? count : 0;
    }
    std::size_t try_pop_batch(T* out, std::size_t max_count) {
        return queue_.try_dequeue_bulk(out, max_count);
    }

    static constexpr std::size_t capacity() { return N; }

private:
    moodycamel::ConcurrentQueue<T> queue_;
};

}  // namespace ringbuffer::bench

#endif  // RINGBUFFER_HAVE_MOODYCAMEL
