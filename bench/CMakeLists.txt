# Benchmark executables. Built only when RINGBUFFER_BUILD_BENCH is ON; never
# run in CI (runner noise makes the numbers meaningless).

find_package(Threads REQUIRED)

# Optional third-party baseline: moodycamel::ConcurrentQueue, fetched only when
# asked for. It's the industry-standard lock-free MPMC queue — the honest
# comparison point. Bench-only; never a dependency of the core library.
if(RINGBUFFER_FETCH_BASELINES)
    include(FetchContent)
    # moodycamel's CMakeLists asks for cmake_minimum_required(3.9), which newer
    # CMake warns is deprecated. Pin a floor for the fetched subproject so the
    # third-party warning doesn't leak into our build. Ignored by older CMake.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    FetchContent_Declare(moodycamel
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        GIT_TAG v1.0.4)
    FetchContent_MakeAvailable(moodycamel)
endif()

# Shared include root so bench sources can say <bench/...> and <ringbuffer/...>.
add_library(ringbuffer_bench_common INTERFACE)
target_include_directories(ringbuffer_bench_common INTERFACE ${CMAKE_SOURCE_DIR})
target_link_libraries(ringbuffer_bench_common
    INTERFACE ringbuffer::ringbuffer Threads::Threads)

function(ringbuffer_add_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ringbuffer_bench_common)
    ringbuffer_configure_target(${name})
    if(RINGBUFFER_FETCH_BASELINES)
        target_include_directories(${name} PRIVATE ${moodycamel_SOURCE_DIR})
        target_compile_definitions(${name} PRIVATE RINGBUFFER_HAVE_MOODYCAMEL)
    endif()
endfunction()

ringbuffer_add_bench(bench_throughput)
ringbuffer_add_bench(bench_latency)
