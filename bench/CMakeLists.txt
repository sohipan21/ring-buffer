# Benchmark executables. Built only when RINGBUFFER_BUILD_BENCH is ON; never
# run in CI (runner noise makes the numbers meaningless).

find_package(Threads REQUIRED)

# Shared include root so bench sources can say <bench/...> and <ringbuffer/...>.
add_library(ringbuffer_bench_common INTERFACE)
target_include_directories(ringbuffer_bench_common INTERFACE ${CMAKE_SOURCE_DIR})
target_link_libraries(ringbuffer_bench_common
    INTERFACE ringbuffer::ringbuffer Threads::Threads)

function(ringbuffer_add_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ringbuffer_bench_common)
    ringbuffer_configure_target(${name})
endfunction()

ringbuffer_add_bench(bench_throughput)
