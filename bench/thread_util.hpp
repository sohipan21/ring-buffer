#pragma once

#include <thread>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ringbuffer::bench {

// Give a benchmark thread the best shot at a consistent core.
//   - Linux: pin to a core (real isolation; the basis for the headline numbers).
//   - macOS: no pinning API, so bias toward the performance cores via QoS. The
//     E/P split and lack of pinning are recorded as a methodology caveat.
inline void configure_bench_thread([[maybe_unused]] unsigned index) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(index % cores, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

}  // namespace ringbuffer::bench
