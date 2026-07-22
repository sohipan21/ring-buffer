#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// A small log-scale histogram for latency percentiles. Buckets are spaced
// geometrically (kSubBuckets per octave), so it covers ~1 ns to ~10 s in a
// few hundred buckets without storing individual samples. Percentiles carry a
// bounded relative error (half a bucket, ~2% at kSubBuckets = 16); min and max
// are tracked exactly.

namespace ringbuffer::bench {

class Histogram {
public:
    void record(std::uint64_t ns) {
        const std::size_t b = bucket_of(ns);
        ++counts_[b];
        ++count_;
        sum_ += ns;
        min_ = std::min(min_, ns);
        max_ = std::max(max_, ns);
    }

    void merge(const Histogram& other) {
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
    }

    // Value at percentile p in [0, 100]. Returns the geometric midpoint of the
    // bucket the cumulative count lands in.
    [[nodiscard]] std::uint64_t percentile(double p) const {
        if (count_ == 0) {
            return 0;
        }
        const std::uint64_t target = static_cast<std::uint64_t>(
            (p / 100.0) * static_cast<double>(count_) + 0.5);
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            cumulative += counts_[i];
            if (cumulative >= target && counts_[i] > 0) {
                return value_of(i);
            }
        }
        return max_;
    }

    [[nodiscard]] std::uint64_t min() const { return count_ ? min_ : 0; }
    [[nodiscard]] std::uint64_t max() const { return max_; }
    [[nodiscard]] std::uint64_t count() const { return count_; }
    [[nodiscard]] double mean() const {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_)
                      : 0.0;
    }

private:
    static constexpr int kSubBuckets = 16;  // per octave
    static constexpr std::size_t kNumBuckets = 640;  // covers past 10^10 ns

    static std::size_t bucket_of(std::uint64_t ns) {
        if (ns <= 1) {
            return 0;
        }
        const double b = std::log2(static_cast<double>(ns)) * kSubBuckets;
        const auto idx = static_cast<std::size_t>(b);
        return std::min(idx, kNumBuckets - 1);
    }

    static std::uint64_t value_of(std::size_t bucket) {
        const double exponent =
            (static_cast<double>(bucket) + 0.5) / kSubBuckets;
        return static_cast<std::uint64_t>(std::exp2(exponent));
    }

    std::vector<std::uint64_t> counts_ =
        std::vector<std::uint64_t>(kNumBuckets, 0);
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace ringbuffer::bench
