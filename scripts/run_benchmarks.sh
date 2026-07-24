#!/usr/bin/env bash
#
# Build the benchmarks in Release and run them, capturing machine specs and
# writing CSVs to results/<date>-<host>/. One command, reproducible.
#
# Usage:
#   scripts/run_benchmarks.sh [LABEL] [EXTRA_CMAKE_ARGS...]
#
#   LABEL             optional CSV suffix, e.g. `seqcst` -> throughput-seqcst.csv
#   EXTRA_CMAKE_ARGS  extra -D flags for configure, e.g. -DRINGBUFFER_FORCE_SEQ_CST=ON
#
# Example (the seq_cst experiment):
#   scripts/run_benchmarks.sh seqcst -DRINGBUFFER_FORCE_SEQ_CST=ON
#
# Sample size is env-overridable for quick runs:
#   RINGBUFFER_BENCH_OPS=500000 RINGBUFFER_BENCH_TRIALS=2 scripts/run_benchmarks.sh

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

label="${1:-}"
[ $# -gt 0 ] && shift || true
extra_cmake_args=("$@")

suffix=""
[ -n "$label" ] && suffix="-$label"

build_dir="build-bench"
out_dir="results/$(date +%F)-$(hostname -s)"
mkdir -p "$out_dir"

# Sample sizes: pre-set env wins so smoke runs stay fast; recorded in specs.txt.
: "${RINGBUFFER_BENCH_OPS:=8000000}"
: "${RINGBUFFER_BENCH_TRIALS:=5}"
: "${RINGBUFFER_BENCH_SAMPLES:=200000}"
: "${RINGBUFFER_BENCH_WARMUP:=20000}"
: "${RINGBUFFER_BENCH_RATE:=2000000}"
export RINGBUFFER_BENCH_OPS RINGBUFFER_BENCH_TRIALS \
    RINGBUFFER_BENCH_SAMPLES RINGBUFFER_BENCH_WARMUP RINGBUFFER_BENCH_RATE

echo ">> configuring ($build_dir)"
cmake -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRINGBUFFER_BUILD_BENCH=ON \
    -DRINGBUFFER_FETCH_BASELINES=ON \
    ${extra_cmake_args[@]+"${extra_cmake_args[@]}"} >/dev/null

echo ">> building"
cmake --build "$build_dir" --parallel >/dev/null

# --- machine specs -----------------------------------------------------------

specs="$out_dir/specs.txt"
cxx="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
{
    echo "date:        $(date)"
    echo "host:        $(hostname)"
    echo "uname:       $(uname -a)"
    echo "git commit:  $(git rev-parse HEAD)$(git diff --quiet || echo ' (dirty)')"
    echo "compiler:    ${cxx:-unknown}"
    [ -n "${cxx:-}" ] && "$cxx" --version | head -1 | sed 's/^/             /'
    echo "cmake:       $(cmake --version | head -1)"
    echo "label:       ${label:-<none>}"
    echo "cmake args:  ${extra_cmake_args[*]+${extra_cmake_args[*]}}"
    echo "bench env:   OPS=$RINGBUFFER_BENCH_OPS TRIALS=$RINGBUFFER_BENCH_TRIALS" \
         "SAMPLES=$RINGBUFFER_BENCH_SAMPLES WARMUP=$RINGBUFFER_BENCH_WARMUP" \
         "RATE=$RINGBUFFER_BENCH_RATE"
    echo ""
    echo "cpu:"
    case "$(uname -s)" in
        Darwin)
            sysctl -n machdep.cpu.brand_string | sed 's/^/  model:       /'
            echo "  logical:     $(sysctl -n hw.ncpu)"
            echo "  perf cores:  $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?')"
            echo "  effic cores: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?')"
            echo "  cache line:  $(sysctl -n hw.cachelinesize) bytes"
            echo "  memory:      $(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GiB"
            ;;
        Linux)
            grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ */  model:      /'
            echo "  logical:     $(nproc)"
            echo "  cache line:  $(getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null || echo '?') bytes"
            ;;
    esac
    echo ""
    echo "conditions:  background load not controlled; single unpinned host run."
    case "$(uname -s)" in
        Darwin) echo "             macOS has no thread pinning (QoS-biased to P-cores); see DESIGN.md." ;;
    esac
} > "$specs"

# --- run ---------------------------------------------------------------------

echo ">> throughput -> $out_dir/throughput$suffix.csv"
"$build_dir/bench/bench_throughput" > "$out_dir/throughput$suffix.csv"

echo ">> latency -> $out_dir/latency$suffix.csv"
"$build_dir/bench/bench_latency" > "$out_dir/latency$suffix.csv" 2> "$out_dir/.latency_stderr"
# Fold the timer-overhead note into the specs.
sed 's/^# /timer:       /' "$out_dir/.latency_stderr" >> "$specs"
rm -f "$out_dir/.latency_stderr"

echo ">> done: $out_dir"
