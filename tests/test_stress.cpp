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
#include <random>
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

// Same guarantees as run_stress, but producers push and consumers pop in
// random-sized batches mixed with single ops — exercises the batch claim/
// partial-return paths under real contention, where the forward-scan probe
// has to cope with out-of-order completion.
template <std::size_t N>
void run_stress_mixed(std::size_t producers, std::size_t consumers,
                      std::size_t items) {
    constexpr std::size_t kMaxBatch = 64;
    ringbuffer::MpmcRingBuffer<std::uint64_t, N> buf;
    const std::size_t total = producers * items;
    std::atomic<std::size_t> consumed{0};

    std::vector<std::vector<std::uint64_t>> sums(
        consumers, std::vector<std::uint64_t>(producers, 0));
    std::vector<std::vector<std::size_t>> counts(
        consumers, std::vector<std::size_t>(producers, 0));

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&buf, p, items] {
            std::mt19937 rng(static_cast<std::uint32_t>(p) + 1);  // seed logged
            std::uniform_int_distribution<std::size_t> batch(1, kMaxBatch);
            std::uint64_t tmp[kMaxBatch];
            std::size_t seq = 0;
            while (seq < items) {
                std::size_t b = std::min(batch(rng), items - seq);
                for (std::size_t j = 0; j < b; ++j) {
                    tmp[j] = pack(static_cast<std::uint32_t>(p),
                                  static_cast<std::uint32_t>(seq + j));
                }
                // Handle partial returns: advance by what was accepted, retry
                // the rest — keeps this producer's items strictly in order.
                std::size_t pushed = 0;
                while (pushed < b) {
                    pushed += buf.try_push_batch(tmp + pushed, b - pushed);
                }
                seq += b;
            }
        });
    }

    for (std::size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            std::mt19937 rng(0x9e3779b9u ^ static_cast<std::uint32_t>(c));
            std::uniform_int_distribution<std::size_t> batch(1, kMaxBatch);
            std::bernoulli_distribution use_single(0.5);
            std::vector<std::int64_t> last(producers, -1);
            std::uint64_t tmp[kMaxBatch];

            auto record = [&](std::uint64_t item) {
                const auto producer = static_cast<std::size_t>(item >> 32);
                const auto s = static_cast<std::uint32_t>(item);
                assert(producer < producers);
                assert(static_cast<std::int64_t>(s) > last[producer]);
                last[producer] = static_cast<std::int64_t>(s);
                sums[c][producer] += s;
                counts[c][producer] += 1;
            };

            while (consumed.load(std::memory_order_relaxed) < total) {
                if (use_single(rng)) {
                    std::uint64_t item = 0;
                    if (buf.try_pop(item)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        record(item);
                    }
                } else {
                    // A popped batch is a contiguous queue segment, so its
                    // items are already in enqueue order — process in order to
                    // keep the per-producer FIFO check valid.
                    const std::size_t got = buf.try_pop_batch(tmp, batch(rng));
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                        for (std::size_t i = 0; i < got; ++i) {
                            record(tmp[i]);
                        }
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

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

    // Mixed batch/single workloads: exercises the batch claim and partial-
    // return paths under contention. Tiny buffer forces mostly-partial batches.
    run_stress_mixed<1024>(4, 4, items);
    run_stress_mixed<4>(4, 4, tiny_items);
    return 0;
}
