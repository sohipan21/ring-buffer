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

// --- batch operations ------------------------------------------------------

void test_batch_full_cycle() {
    ringbuffer::MpmcRingBuffer<int, 32> buf;
    int items[32];
    for (int i = 0; i < 32; ++i) {
        items[i] = i;
    }
    assert(buf.try_push_batch(items, 32) == 32);  // fills exactly
    assert(buf.size_approx() == 32);
    assert(buf.try_push_batch(items, 1) == 0);  // full

    int out[32];
    assert(buf.try_pop_batch(out, 32) == 32);
    for (int i = 0; i < 32; ++i) {
        assert(out[i] == i);  // FIFO
    }
    assert(buf.try_pop_batch(out, 1) == 0);  // empty
    assert(buf.size_approx() == 0);
}

void test_batch_partial_push() {
    ringbuffer::MpmcRingBuffer<int, 32> buf;
    for (int i = 0; i < 27; ++i) {  // leave room for 5
        assert(buf.try_push(i));
    }
    int items[32];
    for (int i = 0; i < 32; ++i) {
        items[i] = 100 + i;
    }
    assert(buf.try_push_batch(items, 32) == 5);  // only 5 free
    assert(buf.size_approx() == 32);

    int out = -1;
    for (int i = 0; i < 27; ++i) {
        assert(buf.try_pop(out));
        assert(out == i);
    }
    for (int i = 0; i < 5; ++i) {
        assert(buf.try_pop(out));
        assert(out == 100 + i);  // the partial batch, in order
    }
}

void test_batch_partial_pop() {
    ringbuffer::MpmcRingBuffer<int, 32> buf;
    for (int i = 0; i < 3; ++i) {
        assert(buf.try_push(i));
    }
    int out[8];
    assert(buf.try_pop_batch(out, 8) == 3);  // only 3 present
    for (int i = 0; i < 3; ++i) {
        assert(out[i] == i);
    }
    assert(buf.size_approx() == 0);
}

void test_batch_of_one_equivalence() {
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    const int in = 42;
    assert(buf.try_push_batch(&in, 1) == 1);
    assert(buf.size_approx() == 1);
    int out = -1;
    assert(buf.try_pop_batch(&out, 1) == 1);
    assert(out == 42);
    assert(buf.try_pop_batch(&out, 1) == 0);
}

void test_batch_zero_count() {
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    int items[4] = {1, 2, 3, 4};
    assert(buf.try_push_batch(items, 0) == 0);
    assert(buf.size_approx() == 0);
    assert(buf.try_push(7));
    int out[4];
    assert(buf.try_pop_batch(out, 0) == 0);
    assert(buf.size_approx() == 1);
}

void test_batch_wraparound() {
    // Move both positions to N-3 (buffer empty), so a batch of 8 straddles the
    // mask wrap.
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    int out = -1;
    for (int i = 0; i < 5; ++i) {  // N-3 = 5 single push/pop cycles
        assert(buf.try_push(i));
        assert(buf.try_pop(out));
        assert(out == i);
    }
    assert(buf.size_approx() == 0);

    int items[8];
    for (int i = 0; i < 8; ++i) {
        items[i] = 200 + i;
    }
    assert(buf.try_push_batch(items, 8) == 8);  // wraps around index boundary
    int drained[8];
    assert(buf.try_pop_batch(drained, 8) == 8);
    for (int i = 0; i < 8; ++i) {
        assert(drained[i] == 200 + i);  // FIFO preserved across the wrap
    }
}

void test_batch_seq_encodes_laps() {
    using WB = ringbuffer::MpmcWhiteBox;
    ringbuffer::MpmcRingBuffer<int, 8> buf;
    int items[5] = {10, 11, 12, 13, 14};
    assert(buf.try_push_batch(items, 5) == 5);
    // Published positions 0..4 → seq p+1.
    for (std::size_t i = 0; i < 5; ++i) {
        assert(WB::seq(buf, i) == i + 1);
    }
    int out[5];
    assert(buf.try_pop_batch(out, 5) == 5);
    // Consumed positions 0..4 → seq p+N.
    for (std::size_t i = 0; i < 5; ++i) {
        assert(WB::seq(buf, i) == i + 8);
    }
}

void test_batch_single_mixed_fifo() {
    // Interleave single and batch ops; global dequeue order must equal the
    // enqueue order regardless of how items were grouped.
    ringbuffer::MpmcRingBuffer<int, 16> buf;
    int next_push = 0;
    int next_pop = 0;
    int out[8];
    for (int round = 0; round < 40; ++round) {
        // Push a small batch, then a single.
        int items[3] = {next_push, next_push + 1, next_push + 2};
        next_push += static_cast<int>(buf.try_push_batch(items, 3));
        if (buf.try_push(next_push)) {
            ++next_push;
        }
        // Pop a single, then a batch.
        if (buf.try_pop(out[0])) {
            assert(out[0] == next_pop++);
        }
        const std::size_t got = buf.try_pop_batch(out, 4);
        for (std::size_t i = 0; i < got; ++i) {
            assert(out[i] == next_pop++);
        }
    }
    // Drain the rest, still in order.
    std::size_t got;
    while ((got = buf.try_pop_batch(out, 8)) > 0) {
        for (std::size_t i = 0; i < got; ++i) {
            assert(out[i] == next_pop++);
        }
    }
    assert(next_pop == next_push);
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

    test_batch_full_cycle();
    test_batch_partial_push();
    test_batch_partial_pop();
    test_batch_of_one_equivalence();
    test_batch_zero_count();
    test_batch_wraparound();
    test_batch_seq_encodes_laps();
    test_batch_single_mixed_fifo();
    return 0;
}
