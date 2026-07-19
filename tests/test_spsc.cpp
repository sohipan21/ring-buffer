// Correctness tests for SpscRingBuffer.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <cstddef>

#include <ringbuffer/spsc_ring_buffer.hpp>

namespace {

void test_pop_on_empty_fails() {
    ringbuffer::SpscRingBuffer<int, 8> buf;
    int out = -1;
    assert(!buf.try_pop(out));
    assert(out == -1);  // untouched on failure
    assert(buf.size_approx() == 0);
}

void test_fill_to_full_then_drain() {
    ringbuffer::SpscRingBuffer<int, 8> buf;
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
    // capacity boundary many times, exercising the mask arithmetic and the
    // monotonic-index full/empty checks far past the first lap.
    for (int fill : {1, 3, 7, 8}) {
        ringbuffer::SpscRingBuffer<int, 8> buf;
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

}  // namespace

int main() {
    static_assert(ringbuffer::SpscRingBuffer<int, 8>::capacity() == 8);

    test_pop_on_empty_fails();
    test_fill_to_full_then_drain();
    test_wraparound_multiple_laps();
    return 0;
}
