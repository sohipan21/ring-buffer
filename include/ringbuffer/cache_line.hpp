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
// GCC warns that this value can differ between translation units built with
// different -mtune, which would be an ABI hazard if it crossed a boundary. It
// doesn't here — every TU builds with the same flags and the value is only used
// for internal alignment — so the warning is suppressed at this one site.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t kCacheLineSize =
    std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

}  // namespace ringbuffer
