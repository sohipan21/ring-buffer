cmake_minimum_required(VERSION 3.20)

project(ringbuffer
    VERSION 0.1.0
    DESCRIPTION "Bounded MPMC ring buffer with batch operations (C++20, header-only)"
    LANGUAGES CXX)

# --- core library: header-only, zero dependencies ----------------------------

add_library(ringbuffer INTERFACE)
add_library(ringbuffer::ringbuffer ALIAS ringbuffer)

target_compile_features(ringbuffer INTERFACE cxx_std_20)
target_include_directories(ringbuffer INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# --- build options ------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(RINGBUFFER_IS_TOP_LEVEL ON)
else()
    set(RINGBUFFER_IS_TOP_LEVEL OFF)
endif()

option(RINGBUFFER_BUILD_TESTS "Build the test suite" ${RINGBUFFER_IS_TOP_LEVEL})
option(RINGBUFFER_BUILD_BENCH "Build the benchmark suite" OFF)
option(RINGBUFFER_FETCH_BASELINES
    "Fetch third-party queues (moodycamel) for benchmark comparison" OFF)

set(RINGBUFFER_SANITIZER "OFF" CACHE STRING
    "Sanitizer for test executables: OFF | thread | address (address also enables undefined)")
set_property(CACHE RINGBUFFER_SANITIZER PROPERTY STRINGS OFF thread address)

option(RINGBUFFER_FORCE_SEQ_CST
    "Force all atomics to seq_cst (to benchmark the cost of the tuned ordering)" OFF)
option(RINGBUFFER_PACKED_SLOTS
    "Pack slots tightly instead of padding each to a cache line" OFF)

# Debug info in every configuration keeps sanitizer and profiler output usable.
if(NOT MSVC)
    add_compile_options(-g)
endif()

# Warnings and sanitizers apply to our own executables only — the INTERFACE
# target carries no flags, so consumers choose their own.
function(ringbuffer_configure_target target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(RINGBUFFER_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    elseif(RINGBUFFER_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if(RINGBUFFER_FORCE_SEQ_CST)
        target_compile_definitions(${target} PRIVATE RINGBUFFER_FORCE_SEQ_CST)
    endif()
    if(RINGBUFFER_PACKED_SLOTS)
        target_compile_definitions(${target} PRIVATE RINGBUFFER_PACKED_SLOTS)
    endif()
endfunction()

# --- tests ----------------------------------------------------------------------

if(RINGBUFFER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- benchmarks -----------------------------------------------------------------

if(RINGBUFFER_BUILD_BENCH)
    add_subdirectory(bench)
endif()
