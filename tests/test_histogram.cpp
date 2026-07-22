// Unit tests for the benchmark histogram. The harness reports latency
// percentiles from this, so it needs to be correct in its own right.

// Tests must also fire in Release builds — keep assert() alive.
#undef NDEBUG
#include <cassert>

#include <cstdint>

#include <bench/histogram.hpp>

namespace {

// The histogram is geometric with 16 sub-buckets per octave, so a reported
// value sits within about ±4% of the true one. Test with a looser 8% margin.
bool near(std::uint64_t got, std::uint64_t want) {
    const double lo = static_cast<double>(want) * 0.92;
    const double hi = static_cast<double>(want) * 1.08;
    return got >= lo && got <= hi;
}

void test_constant_distribution() {
    ringbuffer::bench::Histogram h;
    for (int i = 0; i < 10000; ++i) {
        h.record(1000);
    }
    assert(h.count() == 10000);
    assert(near(h.percentile(50), 1000));
    assert(near(h.percentile(99), 1000));
    assert(h.min() == 1000);  // min/max are exact, not bucketed
    assert(h.max() == 1000);
}

void test_percentiles_monotonic() {
    ringbuffer::bench::Histogram h;
    for (std::uint64_t v = 1; v <= 100000; ++v) {
        h.record(v);
    }
    const auto p50 = h.percentile(50);
    const auto p90 = h.percentile(90);
    const auto p99 = h.percentile(99);
    assert(p50 <= p90);
    assert(p90 <= p99);
    // p50 of a uniform ramp over [1, 100000] is ~50000.
    assert(near(p50, 50000));
    assert(h.max() == 100000);
}

void test_tail_is_visible() {
    // 99% fast (100 ns), 1% slow (1 ms). The point of percentiles: the median
    // stays fast, but the far tail jumps to the slow value.
    ringbuffer::bench::Histogram h;
    for (int i = 0; i < 9900; ++i) {
        h.record(100);
    }
    for (int i = 0; i < 100; ++i) {
        h.record(1000000);
    }
    assert(near(h.percentile(50), 100));    // median unaffected by the 1% tail
    assert(near(h.percentile(99), 100));    // still inside the fast mass at 99
    assert(near(h.percentile(99.5), 1000000));  // past 99% the slow tail shows
    assert(h.max() == 1000000);
}

void test_merge_equivalence() {
    ringbuffer::bench::Histogram a;
    ringbuffer::bench::Histogram b;
    ringbuffer::bench::Histogram whole;
    for (std::uint64_t v = 1; v <= 50000; ++v) {
        a.record(v);
        whole.record(v);
    }
    for (std::uint64_t v = 50001; v <= 100000; ++v) {
        b.record(v);
        whole.record(v);
    }
    a.merge(b);
    assert(a.count() == whole.count());
    assert(a.min() == whole.min());
    assert(a.max() == whole.max());
    assert(a.percentile(50) == whole.percentile(50));
    assert(a.percentile(99) == whole.percentile(99));
}

void test_empty() {
    ringbuffer::bench::Histogram h;
    assert(h.count() == 0);
    assert(h.percentile(50) == 0);
    assert(h.min() == 0);
    assert(h.max() == 0);
}

}  // namespace

int main() {
    test_constant_distribution();
    test_percentiles_monotonic();
    test_tail_is_visible();
    test_merge_equivalence();
    test_empty();
    return 0;
}
