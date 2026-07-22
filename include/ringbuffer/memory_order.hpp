#pragma once

#include <atomic>

namespace ringbuffer::detail {

// The memory orders the buffers actually use. Defining RINGBUFFER_FORCE_SEQ_CST
// collapses all of them to seq_cst, so the cost of the tuned ordering can be
// measured against the conservative baseline (DESIGN.md §6) rather than guessed.
#if defined(RINGBUFFER_FORCE_SEQ_CST)
inline constexpr std::memory_order mo_relaxed = std::memory_order_seq_cst;
inline constexpr std::memory_order mo_acquire = std::memory_order_seq_cst;
inline constexpr std::memory_order mo_release = std::memory_order_seq_cst;
#else
inline constexpr std::memory_order mo_relaxed = std::memory_order_relaxed;
inline constexpr std::memory_order mo_acquire = std::memory_order_acquire;
inline constexpr std::memory_order mo_release = std::memory_order_release;
#endif

}  // namespace ringbuffer::detail
