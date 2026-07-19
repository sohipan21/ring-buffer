#pragma once

#include <cstddef>
#include <new>

namespace ringbuffer {

/// Alignment boundary that prevents destructive interference (false sharing)
/// between hot variables owned by different threads.
///
/// Prefers std::hardware_destructive_interference_size where the standard
/// library ships it. Fallback: 128 on AArch64 — Apple Silicon uses 128-byte
/// cache lines, so the conventional 64 would leave two "separated" variables
/// on one line — and 64 elsewhere. See DESIGN.md §7.
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLineSize =
    std::hardware_destructive_interference_size;
#elif defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

}  // namespace ringbuffer
