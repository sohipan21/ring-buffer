// Single-threaded correctness tests for MpmcRingBuffer: protocol behaviour
// without contention. Concurrent behaviour is covered by test_stress.cpp.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <cstddef>

#include <ringbuffer/mpmc_ring_buffer.hpp>

namespace ringbuffer {

// Test-only access to slot sequence numbers (befriended by the class).
struct MpmcWhiteBox {
    template <typename T, std::size_t N>
    static std::size_t seq(const MpmcRingBuffer<T, N>& buf, std::size_t i) {
        return buf.slots_[i].seq.load(std::memory_order_relaxed);
    }
};

}  // namespace ringbuffer

namespace {

void test_pop_on_empty_fails() {
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    int out = -1;
    assert(!buf.try_pop(out));
    assert(out == -1);  // untouched on failure
    assert(buf.size_approx() == 0);
}

void test_fill_to_full_then_drain() {
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    for (int i = 0; i < 8; ++i) {
        assert(buf.try_push(i));
        assert(buf.size_approx() == static_cast<std::size_t>(i) + 1);
    }
    assert(!buf.try_push(99));  // full — clean failure
    assert(buf.size_approx() == 8);

    int out = -1;
    for (int i = 0; i < 8; ++i) {
        assert(buf.try_pop(out));
        assert(out == i);  // FIFO order preserved
    }
    assert(!buf.try_pop(out));  // empty again
    assert(buf.size_approx() == 0);
}

void test_wraparound_multiple_laps() {
    // Steady-state push/pop at several fill levels; every run crosses the
    // capacity boundary many times, exercising the lap arithmetic well past
    // the first pass over the slot array.
    for (int fill : {1, 3, 7, 8}) {
        ringbuffer::MpmcRingBuffer<int, 8> buf;
        int next_push = 0;
        int next_pop = 0;
        for (; next_push < fill; ++next_push) {
            assert(buf.try_push(next_push));
        }
        // 10 laps of the ring at this fill level.
        for (int step = 0; step < 80; ++step) {
            int out = -1;
            assert(buf.try_pop(out));
            assert(out == next_pop++);
            assert(buf.try_push(next_push++));
            assert(buf.size_approx() == static_cast<std::size_t>(fill));
        }
        // Drain the remainder, still in order.
        int out = -1;
        while (buf.try_pop(out)) {
            assert(out == next_pop++);
        }
        assert(next_pop == next_push);
        assert(buf.size_approx() == 0);
    }
}

void test_interleaved_push_pop() {
    // Push 2, pop 1: the two position counters drift apart and cross the
    // capacity boundary at changing offsets.
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    int next_push = 0;
    int next_pop = 0;
    int out = -1;
    for (int round = 0; round < 64; ++round) {
        if (!buf.try_push(next_push)) {
            // Hit capacity: drain fully and restart the pattern.
            while (buf.try_pop(out)) {
                assert(out == next_pop++);
            }
            assert(buf.try_push(next_push));
        }
        ++next_push;
        if (!buf.try_push(next_push)) {
            while (buf.try_pop(out)) {
                assert(out == next_pop++);
            }
            assert(buf.try_push(next_push));
        }
        ++next_push;
        assert(buf.try_pop(out));
        assert(out == next_pop++);
    }
    while (buf.try_pop(out)) {
        assert(out == next_pop++);
    }
    assert(next_pop == next_push);
}

void test_min_capacity() {
    ringbuffer::MpmcRingBuffer<int, 2> buf;
    static_assert(decltype(buf)::capacity() == 2);
    for (int lap = 0; lap < 5; ++lap) {
        assert(buf.try_push(2 * lap));
        assert(buf.try_push(2 * lap + 1));
        assert(!buf.try_push(-1));  // full at exactly 2
        int out = -1;
        assert(buf.try_pop(out));
        assert(out == 2 * lap);
        assert(buf.try_pop(out));
        assert(out == 2 * lap + 1);
        assert(!buf.try_pop(out));  // empty again
    }
}

void test_seq_encodes_laps() {
    // White-box: the slot sequence numbers are what make reuse safe (ABA),
    // so check they advance exactly as the protocol says.
    using WB = ringbuffer::MpmcWhiteBox;
    ringbuffer::MpmcRingBuffer<int, 8> buf;

    // Fresh buffer: slot i is free for position i.
    for (std::size_t i = 0; i < 8; ++i) {
        assert(WB::seq(buf, i) == i);
    }
    // Publishing position p sets its slot to p + 1.
    for (int i = 0; i < 8; ++i) {
        assert(buf.try_push(i));
        assert(WB::seq(buf, static_cast<std::size_t>(i)) ==
               static_cast<std::size_t>(i) + 1);
    }
    // Consuming position p sets its slot to p + N: free for the next lap.
    int out = -1;
    for (std::size_t i = 0; i < 8; ++i) {
        assert(buf.try_pop(out));
        assert(WB::seq(buf, i) == i + 8);
    }
    // Second lap publishes at pos 8 → seq 9.
    assert(buf.try_push(100));
    assert(WB::seq(buf, 0) == 9);
}

}  // namespace

int main() {
    static_assert(ringbuffer::MpmcRingBuffer<int, 8>::capacity() == 8);

    test_pop_on_empty_fails();
    test_fill_to_full_then_drain();
    test_wraparound_multiple_laps();
    test_interleaved_push_pop();
    test_min_capacity();
    test_seq_encodes_laps();
    return 0;
}
