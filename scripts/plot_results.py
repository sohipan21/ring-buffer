#!/usr/bin/env python3
"""Plot batch size vs throughput from a benchmark CSV.

One line per producer/consumer config for the MPMC queue — the batch-
amortisation curve from DESIGN.md section 12.

Usage: python3 scripts/plot_results.py <throughput.csv> <out.png>
"""

import csv
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")  # no display needed
import matplotlib.pyplot as plt


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    csv_path, out_path = sys.argv[1], sys.argv[2]

    # (producers, consumers, batch) -> list of ops/sec across trials
    trials = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row["queue"] != "mpmc":
                continue
            key = (int(row["producers"]), int(row["consumers"]), int(row["batch"]))
            trials[key].append(float(row["ops_per_sec"]))

    # median ops/sec per config, in millions
    configs = sorted({(p, c) for (p, c, _) in trials})
    batches = sorted({b for (_, _, b) in trials})

    fig, ax = plt.subplots(figsize=(7, 4.5))
    for p, c in configs:
        ys = [statistics.median(trials[(p, c, b)]) / 1e6 for b in batches]
        ax.plot(batches, ys, marker="o", label=f"{p}P / {c}C")

    ax.set_xscale("log", base=2)
    ax.set_xticks(batches)
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.set_xlabel("batch size (elements per call)")
    ax.set_ylabel("throughput (million ops/sec, median)")
    ax.set_title("MPMC throughput vs batch size — Apple M2 Pro (unpinned)")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend(title="threads")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
