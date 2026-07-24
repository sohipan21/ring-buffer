#!/usr/bin/env bash
#
# Turn a benchmark CSV into a markdown table on stdout. Auto-detects throughput
# vs latency by the header. Reads only committed CSVs, so it can't produce a
# number that isn't in the data.
#
# Usage: scripts/summarize_results.sh <csv>

set -euo pipefail

csv="${1:?usage: summarize_results.sh <csv>}"
header="$(head -1 "$csv")"

case "$header" in
    queue,producers,consumers,batch,trial,ops_per_sec)
        # Median ops/sec per config across trials. Flat SUBSEP-keyed arrays keep
        # this portable to the BSD awk on macOS (no gawk-only nested arrays).
        echo "| queue | P/C | batch | median ops/s | min | max |"
        echo "|---|---|---|--:|--:|--:|"
        awk -F, '
            NR == 1 { next }
            {
                key = $1 "|" $2 "/" $3 "|" $4
                c = ++n[key]
                vals[key, c] = $6 + 0
                q[key] = $1; pc[key] = $2 "/" $3; b[key] = $4
            }
            END {
                for (k in n) {
                    c = n[k]
                    for (i = 1; i <= c; i++) t[i] = vals[k, i]
                    for (i = 2; i <= c; i++) {  # insertion sort t[1..c]
                        v = t[i]; j = i - 1
                        while (j >= 1 && t[j] > v) { t[j+1] = t[j]; j-- }
                        t[j+1] = v
                    }
                    med = (c % 2) ? t[(c+1)/2] : int((t[c/2] + t[c/2+1]) / 2)
                    printf "| %s | %s | %s | %d | %d | %d |\n",
                           q[k], pc[k], b[k], med, t[1], t[c]
                }
            }
        ' "$csv" | sort
        ;;
    queue,shape,load,samples,*)
        # Latency rows are already percentiles — just format them.
        awk -F, '
            NR == 1 { next }
            NR == 2 {
                print "| queue | shape | load | p50 ns | p99 ns | p99.9 ns | max ns |"
                print "|---|---|---|--:|--:|--:|--:|"
            }
            { printf "| %s | %s | %s | %s | %s | %s | %s |\n",
                     $1, $2, $3, $5, $6, $7, $8 }
        ' "$csv"
        ;;
    *)
        echo "unrecognised CSV header: $header" >&2
        exit 1
        ;;
esac
