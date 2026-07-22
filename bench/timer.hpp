#pragma once

#include <chrono>
#include <cstdint>

// Nanosecond clock for the benchmarks. steady_clock everywhere by default —
// on macOS that's mach_absolute_time, cheap and ~ns-resolution. An x86-64
// rdtscp path (calibrated against steady_clock) is available behind
// RINGBUFFER_BENCH_RDTSCP for the Linux runs; ARM has no equivalent user-space
// cycle counter worth using here, so it always takes the steady_clock path.

#if defined(RINGBUFFER_BENCH_RDTSCP) && defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace ringbuffer::bench {

#if defined(RINGBUFFER_BENCH_RDTSCP) && defined(__x86_64__)

// Cycles-per-nanosecond, calibrated once against steady_clock. Assumes an
// invariant TSC (constant rate regardless of frequency scaling) — true on any
// modern x86-64; the calibration is only valid under that assumption.
inline double tsc_ghz() {
    static const double ghz = [] {
        unsigned aux;
        const std::uint64_t t0 = __rdtscp(&aux);
        const auto c0 = std::chrono::steady_clock::now();
        // Busy-wait ~10 ms so the ratio is stable.
        while (std::chrono::steady_clock::now() - c0 <
               std::chrono::milliseconds(10)) {
        }
        const std::uint64_t t1 = __rdtscp(&aux);
        const auto c1 = std::chrono::steady_clock::now();
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0)
                .count();
        return static_cast<double>(t1 - t0) / static_cast<double>(ns);
    }();
    return ghz;
}

inline std::uint64_t now_ns() {
    unsigned aux;
    return static_cast<std::uint64_t>(static_cast<double>(__rdtscp(&aux)) /
                                      tsc_ghz());
}

#else

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
}

#endif

// Rough per-call cost of now_ns(), so latency numbers can be read against the
// clock's own floor. Reported into the results, not subtracted out.
inline double timer_overhead_ns() {
    constexpr int kIters = 200000;
    const std::uint64_t start = now_ns();
    std::uint64_t sink = 0;
    for (int i = 0; i < kIters; ++i) {
        sink += now_ns();
    }
    const std::uint64_t end = now_ns();
    // Keep `sink` observable so the loop isn't optimised away.
    volatile std::uint64_t keep = sink;
    (void)keep;
    return static_cast<double>(end - start) / kIters;
}

}  // namespace ringbuffer::bench
