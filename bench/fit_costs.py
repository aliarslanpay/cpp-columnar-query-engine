"""Fit the fused scan model from one reference-verified cqe_calibrate CSV."""

import csv
import sys
from collections import defaultdict
from statistics import median


def balanced_median(observations):
    # Give range and equality equal weight when both have an observation at the anchor.
    groups = defaultdict(list)
    for scenario, time in observations:
        groups[scenario.split("_")[0]].append(time)
    return median(median(values) for values in groups.values())


def fit(path):
    samples = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            key = (row["scenario"], row["operation"], int(row["thread_budget"]),
                   int(row["candidate_blocks"]))
            samples[key].append(int(row["elapsed_ns"]) / int(row["repetitions"]))
    times = {key: median(values) for key, values in samples.items()}
    dense = max(blocks for _, _, _, blocks in times)
    full = [(scenario, time / dense) for (scenario, op, workers, blocks), time in times.items()
            if op == "full" and workers == 1 and blocks == dense]
    print(f"Serial contiguous full scan: {balanced_median(full):.2f} ns/block")

    discovery = {"zone_map": "zone_candidates", "index": "index_candidates"}
    for workers in (1, 2, 4, 8):
        by_blocks = defaultdict(list)
        for (scenario, op, budget, blocks), time in times.items():
            if budget != workers or blocks < workers:
                continue
            if op in discovery:
                time -= times[scenario, discovery[op], 1, blocks]
            elif op != "full" or workers == 1:
                continue
            by_blocks[blocks].append((scenario, time))

        # Anchor the line at the smallest workload that uses the requested team and at
        # the dense workload. Pool equivalent block executors at each endpoint.
        small = min(by_blocks)
        small_ns = balanced_median(by_blocks[small])
        dense_ns = balanced_median(by_blocks[dense])
        slope = (dense_ns - small_ns) / (dense - small)
        setup = max(0.0, small_ns - slope * small)
        print(f"{workers} workers: anchors ({small}, {small_ns:.2f}), "
              f"({dense}, {dense_ns:.2f}), setup {setup:.2f} ns, {slope:.2f} ns/block")


if __name__ == "__main__":
    fit(sys.argv[1])
