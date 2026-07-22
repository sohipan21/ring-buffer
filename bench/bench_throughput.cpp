// Throughput benchmark: how many push+pop operations per second a queue
// sustains under P producers and C consumers, for single ops and for batches.
//
// Method: all worker threads meet at a barrier so measured work starts
// together; one worker stamps the start, each consumer stamps when it finishes,
// and throughput is total ops over that span. Warmup discarded; several trials;
// median with min/max reported. Output is CSV on stdout.

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <ringbuffer/mpmc_ring_buffer.hpp>
#include <ringbuffer/spsc_ring_buffer.hpp>

#include <bench/barrier.hpp>
#include <bench/baselines/mutex_queue.hpp>
#include <bench/queue_adapters.hpp>
#include <bench/thread_util.hpp>
#include <bench/timer.hpp>

namespace {

using ringbuffer::bench::configure_bench_thread;
using ringbuffer::bench::now_ns;
using ringbuffer::bench::SpinBarrier;

constexpr std::size_t kCapacity = 1024;
constexpr std::size_t kMaxBatch = 64;

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    if (const char* v = std::getenv(name)) {
        return std::strtoull(v, nullptr, 10);
    }
    return fallback;
}

// Pack producer id and index so items are distinct; the benchmark doesn't check
// them, but distinct values keep the optimiser from folding the stores away.
std::uint64_t pack(std::size_t p, std::uint64_t i) {
    return (static_cast<std::uint64_t>(p) << 40) | i;
}

// Not every queue under test has a batch API (SPSC doesn't); the batch code
// path is compiled only for those that do.
template <typename Q>
concept HasBatch = requires(Q q, std::uint64_t* out, const std::uint64_t* in,
                            std::size_t n) {
    { q.try_push_batch(in, n) } -> std::convertible_to<std::size_t>;
    { q.try_pop_batch(out, n) } -> std::convertible_to<std::size_t>;
};

template <typename Queue>
double ops_per_sec(std::size_t producers, std::size_t consumers,
                   std::uint64_t total_ops, std::size_t batch) {
    Queue queue;
    const std::uint64_t per_producer = total_ops / producers;
    const std::uint64_t actual_total = per_producer * producers;

    SpinBarrier barrier(static_cast<int>(producers + consumers));
    std::atomic<std::uint64_t> start_ns{0};
    std::atomic_flag started = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::uint64_t> consumer_end(consumers, 0);

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            configure_bench_thread(static_cast<unsigned>(p));
            std::array<std::uint64_t, kMaxBatch> buf;
            barrier.arrive_and_wait();
            if (!started.test_and_set()) {
                start_ns.store(now_ns(), std::memory_order_relaxed);
            }
            std::uint64_t i = 0;
            while (i < per_producer) {
                if (batch <= 1) {
                    while (!queue.try_push(pack(p, i))) {
                    }
                    ++i;
                } else if constexpr (HasBatch<Queue>) {
                    const std::size_t b = static_cast<std::size_t>(
                        std::min<std::uint64_t>(batch, per_producer - i));
                    for (std::size_t j = 0; j < b; ++j) {
                        buf[j] = pack(p, i + j);
                    }
                    std::size_t pushed = 0;
                    while (pushed < b) {
                        pushed += queue.try_push_batch(buf.data() + pushed, b - pushed);
                    }
                    i += b;
                }
            }
        });
    }

    for (std::size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            configure_bench_thread(static_cast<unsigned>(producers + c));
            std::array<std::uint64_t, kMaxBatch> buf;
            barrier.arrive_and_wait();
            while (consumed.load(std::memory_order_relaxed) < actual_total) {
                if (batch <= 1) {
                    std::uint64_t out;
                    if (queue.try_pop(out)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if constexpr (HasBatch<Queue>) {
                    const std::size_t got = queue.try_pop_batch(buf.data(), batch);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            }
            consumer_end[c] = now_ns();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    const std::uint64_t end = *std::max_element(consumer_end.begin(), consumer_end.end());
    const double elapsed_ns = static_cast<double>(end - start_ns.load());
    return static_cast<double>(actual_total) * 1e9 / elapsed_ns;
}

// Runs warmup + trials for one config and prints a CSV row per trial.
template <typename Queue>
void measure(const char* queue_name, std::size_t producers, std::size_t consumers,
             std::size_t batch, std::uint64_t total_ops, int trials) {
    (void)ops_per_sec<Queue>(producers, consumers, total_ops, batch);  // warmup
    for (int t = 0; t < trials; ++t) {
        const double ops = ops_per_sec<Queue>(producers, consumers, total_ops, batch);
        std::printf("%s,%zu,%zu,%zu,%d,%.0f\n", queue_name, producers, consumers,
                    batch, t, ops);
        std::fflush(stdout);
    }
}

using Mpmc = ringbuffer::MpmcRingBuffer<std::uint64_t, kCapacity>;
using Spsc = ringbuffer::SpscRingBuffer<std::uint64_t, kCapacity>;
using Mutex = ringbuffer::bench::MutexQueue<std::uint64_t, kCapacity>;

}  // namespace

int main() {
    const std::uint64_t ops = env_u64("RINGBUFFER_BENCH_OPS", 8'000'000);
    const int trials = static_cast<int>(env_u64("RINGBUFFER_BENCH_TRIALS", 5));

    std::printf("queue,producers,consumers,batch,trial,ops_per_sec\n");

    const std::size_t thread_configs[][2] = {{1, 1}, {2, 2}, {4, 4}};

#if defined(RINGBUFFER_HAVE_MOODYCAMEL)
    using Moody = ringbuffer::bench::MoodycamelQueue<std::uint64_t, kCapacity>;
#endif

    // Single-op throughput.
    for (const auto& cfg : thread_configs) {
        measure<Mpmc>("mpmc", cfg[0], cfg[1], 1, ops, trials);
        measure<Mutex>("mutex", cfg[0], cfg[1], 1, ops, trials);
#if defined(RINGBUFFER_HAVE_MOODYCAMEL)
        measure<Moody>("moodycamel", cfg[0], cfg[1], 1, ops, trials);
#endif
    }
    measure<Spsc>("spsc", 1, 1, 1, ops, trials);  // SPSC is 1P/1C only

    // Batch throughput across batch sizes (queues with a batch API).
    const std::size_t batches[] = {4, 8, 16, 32, 64};
    for (const auto& cfg : thread_configs) {
        for (std::size_t b : batches) {
            measure<Mpmc>("mpmc", cfg[0], cfg[1], b, ops, trials);
            measure<Mutex>("mutex", cfg[0], cfg[1], b, ops, trials);
#if defined(RINGBUFFER_HAVE_MOODYCAMEL)
            measure<Moody>("moodycamel", cfg[0], cfg[1], b, ops, trials);
#endif
        }
    }

    return 0;
}
