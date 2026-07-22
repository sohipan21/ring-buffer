// Latency benchmark: how long a single operation takes, not how many per
// second. Two shapes, reported separately because they answer different
// questions:
//
//   pingpong  — round-trip through two queues, one item in flight. Pure service
//               latency, no queueing.
//   e2e       — producer stamps each item with now_ns(), consumer measures the
//               delta at pop. Run saturated (throughput-bound, so latency is
//               dominated by time spent sitting in the queue) and at a moderate
//               open-loop rate (queue near-empty, closer to service latency).
//
// Coordinated omission caveat: under saturation the producer blocks when the
// queue is full, so stamps aren't taken on an independent schedule — the
// saturated numbers describe in-queue residency, not offered-load latency.
// That's why both shapes are shown.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <ringbuffer/mpmc_ring_buffer.hpp>
#include <ringbuffer/spsc_ring_buffer.hpp>

#include <bench/baselines/mutex_queue.hpp>
#include <bench/histogram.hpp>
#include <bench/queue_adapters.hpp>
#include <bench/thread_util.hpp>
#include <bench/timer.hpp>

namespace {

using ringbuffer::bench::configure_bench_thread;
using ringbuffer::bench::Histogram;
using ringbuffer::bench::now_ns;

constexpr std::size_t kCapacity = 1024;

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    if (const char* v = std::getenv(name)) {
        return std::strtoull(v, nullptr, 10);
    }
    return fallback;
}

// One item in flight, bounced client -> server -> client; the client times the
// round trip. Needs a queue each way.
template <typename Queue>
Histogram ping_pong(std::uint64_t samples, std::uint64_t warmup) {
    Queue to_server;
    Queue to_client;
    Histogram h;

    std::thread server([&] {
        configure_bench_thread(1);
        std::uint64_t token;
        for (std::uint64_t i = 0; i < samples + warmup; ++i) {
            while (!to_server.try_pop(token)) {
            }
            while (!to_client.try_push(token)) {
            }
        }
    });

    configure_bench_thread(0);
    std::uint64_t token;
    for (std::uint64_t i = 0; i < samples + warmup; ++i) {
        const std::uint64_t t0 = now_ns();
        while (!to_server.try_push(i)) {
        }
        while (!to_client.try_pop(token)) {
        }
        const std::uint64_t t1 = now_ns();
        if (i >= warmup) {
            h.record(t1 - t0);
        }
    }
    server.join();
    return h;
}

// Producer stamps now_ns() into each item; consumer records now_ns() - stamp.
// rate == 0 means saturate; otherwise pace to ~rate ops/sec (open loop).
template <typename Queue>
Histogram end_to_end(std::uint64_t samples, std::uint64_t warmup,
                     std::uint64_t rate) {
    Queue queue;
    Histogram h;
    const std::uint64_t need = samples + warmup;

    std::thread consumer([&] {
        configure_bench_thread(1);
        std::uint64_t item;
        std::uint64_t got = 0;
        while (got < need) {
            if (queue.try_pop(item)) {
                const std::uint64_t now = now_ns();
                if (got >= warmup) {
                    h.record(now - item);
                }
                ++got;
            }
        }
    });

    configure_bench_thread(0);
    const std::uint64_t interval = rate ? (1'000'000'000ull / rate) : 0;
    std::uint64_t next = now_ns();
    for (std::uint64_t i = 0; i < need; ++i) {
        if (interval) {
            while (now_ns() < next) {
            }
            next += interval;
        }
        const std::uint64_t stamp = now_ns();
        while (!queue.try_push(stamp)) {
        }
    }
    consumer.join();
    return h;
}

void report(const char* queue, const char* shape, const char* load,
            const Histogram& h) {
    std::printf("%s,%s,%s,%llu,%llu,%llu,%llu,%llu\n", queue, shape, load,
                static_cast<unsigned long long>(h.count()),
                static_cast<unsigned long long>(h.percentile(50)),
                static_cast<unsigned long long>(h.percentile(99)),
                static_cast<unsigned long long>(h.percentile(99.9)),
                static_cast<unsigned long long>(h.max()));
    std::fflush(stdout);
}

template <typename Queue>
void run_all(const char* name, std::uint64_t samples, std::uint64_t warmup,
             std::uint64_t moderate_rate) {
    report(name, "pingpong", "na", ping_pong<Queue>(samples, warmup));
    report(name, "e2e", "saturated", end_to_end<Queue>(samples, warmup, 0));
    report(name, "e2e", "moderate",
           end_to_end<Queue>(samples, warmup, moderate_rate));
}

using Mpmc = ringbuffer::MpmcRingBuffer<std::uint64_t, kCapacity>;
using Spsc = ringbuffer::SpscRingBuffer<std::uint64_t, kCapacity>;
using Mutex = ringbuffer::bench::MutexQueue<std::uint64_t, kCapacity>;

}  // namespace

int main() {
    const std::uint64_t samples = env_u64("RINGBUFFER_BENCH_SAMPLES", 200'000);
    const std::uint64_t warmup = env_u64("RINGBUFFER_BENCH_WARMUP", 20'000);
    const std::uint64_t moderate = env_u64("RINGBUFFER_BENCH_RATE", 2'000'000);

    std::printf("queue,shape,load,samples,p50_ns,p99_ns,p999_ns,max_ns\n");
    std::fprintf(stderr, "# timer overhead ~%.1f ns/call\n",
                 ringbuffer::bench::timer_overhead_ns());

    run_all<Mpmc>("mpmc", samples, warmup, moderate);
    run_all<Spsc>("spsc", samples, warmup, moderate);
    run_all<Mutex>("mutex", samples, warmup, moderate);
#if defined(RINGBUFFER_HAVE_MOODYCAMEL)
    run_all<ringbuffer::bench::MoodycamelQueue<std::uint64_t, kCapacity>>(
        "moodycamel", samples, warmup, moderate);
#endif
    return 0;
}
