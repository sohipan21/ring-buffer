#pragma once

#include <atomic>

namespace ringbuffer::bench {

// A reusable spin barrier so every benchmark thread starts its measured work at
// the same instant — without it, threads that start early inflate the apparent
// throughput. Generation-counted so one barrier can be reused across trials.
class SpinBarrier {
public:
    explicit SpinBarrier(int n) : count_(n), total_(n) {}

    void arrive_and_wait() {
        const int gen = generation_.load(std::memory_order_acquire);
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count_.store(total_, std::memory_order_relaxed);  // reset for reuse
            generation_.fetch_add(1, std::memory_order_release);  // release all
        } else {
            while (generation_.load(std::memory_order_acquire) == gen) {
                // spin until the last thread bumps the generation
            }
        }
    }

private:
    std::atomic<int> count_;
    std::atomic<int> generation_{0};
    int total_;
};

}  // namespace ringbuffer::bench
