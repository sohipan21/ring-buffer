// Multi-threaded stress tests for MpmcRingBuffer: exactly-once consumption
// and per-producer FIFO order under contention.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include <ringbuffer/mpmc_ring_buffer.hpp>

namespace {

// Every item is self-identifying: producer id in the high half, that
// producer's sequence number in the low half.
constexpr std::uint64_t pack(std::uint32_t producer, std::uint32_t seq) {
    return (static_cast<std::uint64_t>(producer) << 32) | seq;
}

std::size_t items_per_producer() {
    // CI overrides this downward — sanitizer builds run 5-20x slower.
    if (const char* env = std::getenv("RINGBUFFER_STRESS_ITEMS")) {
        return static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
    }
    return 1'000'000;
}

template <std::size_t N>
void run_stress(std::size_t producers, std::size_t consumers,
                std::size_t items) {
    ringbuffer::MpmcRingBuffer<std::uint64_t, N> buf;
    const std::size_t total = producers * items;
    std::atomic<std::size_t> consumed{0};

    // Per-consumer tallies, aggregated after the join (no sharing while the
    // threads run).
    std::vector<std::vector<std::uint64_t>> sums(
        consumers, std::vector<std::uint64_t>(producers, 0));
    std::vector<std::vector<std::size_t>> counts(
        consumers, std::vector<std::size_t>(producers, 0));

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&buf, p, items] {
            for (std::size_t s = 0; s < items; ++s) {
                const std::uint64_t item = pack(static_cast<std::uint32_t>(p),
                                                static_cast<std::uint32_t>(s));
                while (!buf.try_push(item)) {
                }
            }
        });
    }

    for (std::size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            // Last seq_no this consumer saw from each producer. Must only
            // ever increase: pops by one consumer happen in queue order, and
            // one producer's items enter the queue in seq order.
            std::vector<std::int64_t> last(producers, -1);
            while (consumed.load(std::memory_order_relaxed) < total) {
                std::uint64_t item = 0;
                if (!buf.try_pop(item)) {
                    continue;
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
                const auto producer = static_cast<std::size_t>(item >> 32);
                const auto seq = static_cast<std::uint32_t>(item);
                assert(producer < producers);
                assert(static_cast<std::int64_t>(seq) > last[producer]);
                last[producer] = static_cast<std::int64_t>(seq);
                sums[c][producer] += seq;
                counts[c][producer] += 1;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Exactly-once: per producer, the count catches loss and duplication,
    // the sum catches substituted/corrupted values.
    for (std::size_t p = 0; p < producers; ++p) {
        std::size_t count = 0;
        std::uint64_t sum = 0;
        for (std::size_t c = 0; c < consumers; ++c) {
            count += counts[c][p];
            sum += sums[c][p];
        }
        assert(count == items);
        assert(sum == static_cast<std::uint64_t>(items) * (items - 1) / 2);
    }
    assert(buf.size_approx() == 0);
}

}  // namespace

int main() {
    const std::size_t items = items_per_producer();

    run_stress<1024>(1, 1, items);
    run_stress<1024>(2, 2, items);
    run_stress<1024>(4, 4, items);

    // Tiny buffer, 8 threads: every operation sits on a full/empty boundary,
    // so claim races and lap transitions are constant.
    const std::size_t tiny_items = std::max<std::size_t>(items / 10, 1000);
    run_stress<4>(4, 4, tiny_items);
    return 0;
}
