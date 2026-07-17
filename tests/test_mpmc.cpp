// Single-threaded correctness tests for MpmcRingBuffer.
//
// Currently a scaffold: verifies the public API compiles, links, and holds its
// compile-time contracts. Behavioural tests land with the protocol.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <ringbuffer/mpmc_ring_buffer.hpp>

int main() {
    using Buffer = ringbuffer::MpmcRingBuffer<int, 8>;
    static_assert(Buffer::capacity() == 8);

    Buffer buf;
    assert(buf.size_approx() == 0);

    int out = 0;
    (void)buf.try_push(42);
    (void)buf.try_pop(out);

    const int items[4] = {1, 2, 3, 4};
    int drained[4] = {};
    (void)buf.try_push_batch(items, 4);
    (void)buf.try_pop_batch(drained, 4);

    return 0;
}
