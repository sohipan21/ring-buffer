// Correctness tests for SpscRingBuffer.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <cstddef>
#include <cstdint>
#include <thread>

#include <ringbuffer/cache_line.hpp>
#include <ringbuffer/spsc_ring_buffer.hpp>

namespace ringbuffer {

// Test-only access to private layout (befriended by the class).
struct SpscWhiteBox {
    template <typename T, std::size_t N>
    static void check_alignment(const SpscRingBuffer<T, N>& buf) {
        auto line = [](const void* p) {
            return reinterpret_cast<std::uintptr_t>(p) / kCacheLineSize;
        };
        // Producer's index, consumer's index, and the data each own a line.
        assert(line(&buf.head_) != line(&buf.tail_));
        assert(line(&buf.head_) != line(&buf.data_[0]));
        assert(line(&buf.tail_) != line(&buf.data_[0]));
    }
};

}  // namespace ringbuffer

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

// One producer thread streams 1M sequential ints while one consumer drains.
// The strict-sequence assert is the whole proof for SPSC: any lost, duplicated
// or reordered item breaks it immediately. The checksum catches value
// corruption independently of ordering.
template <std::size_t N>
void test_two_thread_transfer() {
    constexpr int kItems = 1'000'000;
    ringbuffer::SpscRingBuffer<int, N> buf;
    std::uint64_t consumer_sum = 0;

    std::thread producer([&buf] {
        for (int i = 0; i < kItems; ++i) {
            while (!buf.try_push(i)) {
            }
        }
    });
    std::thread consumer([&buf, &consumer_sum] {
        int expected = 0;
        std::uint64_t sum = 0;
        while (expected < kItems) {
            int out = -1;
            if (buf.try_pop(out)) {
                assert(out == expected);
                ++expected;
                sum += static_cast<std::uint64_t>(out);
            }
        }
        consumer_sum = sum;
    });
    producer.join();
    consumer.join();

    constexpr auto kExpectedSum =
        static_cast<std::uint64_t>(kItems) * (kItems - 1) / 2;
    assert(consumer_sum == kExpectedSum);
    assert(buf.size_approx() == 0);
}

void test_alignment() {
    ringbuffer::SpscRingBuffer<int, 8> buf;
    ringbuffer::SpscWhiteBox::check_alignment(buf);
}

}  // namespace

int main() {
    static_assert(ringbuffer::SpscRingBuffer<int, 8>::capacity() == 8);

    test_pop_on_empty_fails();
    test_fill_to_full_then_drain();
    test_wraparound_multiple_laps();
    test_alignment();

    test_two_thread_transfer<1024>();
    test_two_thread_transfer<4>();  // tiny buffer: constant boundary races
    return 0;
}
