"""Print the median tables of docs/MEASUREMENTS.md from raw benchmark CSV files.

Usage: python3 bench/summarize_results.py [--scan CSV] [--append CSV] [--ordering CSV]

The defaults are the committed files results/scan-calibration-final.csv (cqe_calibrate output),
results/append-maintenance.csv (cqe_bench_append output) and results/ordering-pruning.csv
(cqe_bench_ordering output). Standard library only. Every query
time is the median over the samples of one configuration of elapsed_ns / repetitions, shown with
the minimum and maximum of the same per-call values.
"""

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median


def load_scan(path):
    """Map (scenario, operation, thread_budget) to per-call times and candidate block counts."""
    times = defaultdict(list)
    blocks = defaultdict(set)
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            key = (row["scenario"], row["operation"], int(row["thread_budget"]))
            times[key].append(int(row["elapsed_ns"]) / int(row["repetitions"]))
            blocks[key].add(int(row["candidate_blocks"]))
    return times, blocks


def cell(values, digits=0):
    return f"{median(values):.{digits}f} [{min(values):.{digits}f}, {max(values):.{digits}f}]"


def only(values):
    (value,) = values
    return value


def print_equality(times, blocks, budget):
    print(f"Equality predicates, worker budget {budget}: median ns per query [min, max]")
    print("| Device | Index candidates | Full | Zone map | Index | Auto | Auto blocks scanned |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for scenario in ("device_common", "device_rare", "device_absent"):
        print(f"| {scenario.removeprefix('device_')} "
              f"| {only(blocks[scenario, 'index_candidates', 1])} "
              f"| {cell(times[scenario, 'full', budget])} "
              f"| {cell(times[scenario, 'zone_map', budget])} "
              f"| {cell(times[scenario, 'index', budget])} "
              f"| {cell(times[scenario, 'auto', budget])} "
              f"| {only(blocks[scenario, 'auto', budget])} |")
    print()


def print_discovery(times, blocks):
    print("Candidate discovery alone (serial): median ns per call [min, max]")
    print("| Scenario | Zone-map candidates | Zone-map pass | Index candidates | Index lookup |")
    print("|---|---:|---:|---:|---:|")
    scenarios = sorted({scenario for scenario, _, _ in times},
                       key=lambda s: (s.startswith("range_"), int(s.split("_")[1])
                                      if s.startswith("range_") else s))
    for scenario in scenarios:
        zone = (scenario, "zone_candidates", 1)
        index = (scenario, "index_candidates", 1)
        index_cells = (f"{only(blocks[index])} | {cell(times[index])}" if index in times
                       else "n/a | n/a")
        print(f"| {scenario} | {only(blocks[zone])} | {cell(times[zone])} | {index_cells} |")
    print()


def print_ranges(times, blocks, budgets):
    print("Timestamp ranges on time-ordered rows: median ns per query [min, max]")
    header = " | ".join(f"{op} b={budget}" for op in ("full", "zone_map") for budget in budgets)
    print(f"| Scenario | Zone-map blocks | {header} |")
    print("|---|---:|" + "---:|" * (2 * len(budgets)))
    scenarios = sorted({s for s, _, _ in times if s.startswith("range_")},
                       key=lambda s: int(s.split("_")[1]))
    for scenario in scenarios:
        cells = " | ".join(cell(times[scenario, op, budget])
                           for op in ("full", "zone_map") for budget in budgets)
        print(f"| {scenario} | {only(blocks[scenario, 'zone_map', 1])} | {cells} |")
    print()


def print_ordering(path):
    times = defaultdict(list)
    shape = {}
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            key = (row["ordering"], row["operation"])
            times[key].append(int(row["elapsed_ns"]) / int(row["repetitions"]))
            shape.setdefault(key, set()).add((row["blocks_scanned"], row["blocks_total"],
                                              row["rows_examined"], row["rows_matched"]))
    print("One narrow timestamp range, worker budget 1: median ns per query [min, max]")
    print("| Ordering | Scan | Blocks scanned | Rows examined | Rows matched | Time |")
    print("|---|---|---:|---:|---:|---:|")
    for key in sorted(times):
        scanned, total, examined, matched = only(shape[key])
        print(f"| {key[0]} | {key[1]} | {scanned} / {total} | {examined} | {matched} "
              f"| {cell(times[key])} |")
    print()


def print_append(path):
    batches = defaultdict(list)
    sequences = defaultdict(list)
    batch_sums = defaultdict(lambda: [0, 0, 0, 0])
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            key = row["scenario"]
            sample = int(row["sample"])
            if row["scope"] == "batch":
                batches[key].append(row)
                total = batch_sums[key, sample]
                total[0] += int(row["maintain_ns"])
                total[1] += int(row["rebuild_ns"])
                total[2] += int(row["merges"])
                total[3] += int(row["added_rows"])
            else:
                sequences[key].append(row)
                expected = [int(row["maintain_ns"]), int(row["rebuild_ns"]), int(row["merges"]),
                            int(row["added_rows"])]
                if batch_sums[key, sample] != expected:
                    raise SystemExit(f"{path}: {key} sample {sample} does not match its batches")

    print("Append with maintenance vs full rebuild: median ns [min, max]")
    print("| Scenario | Rows per batch | Batches | Merges per sequence | Batch append | "
          "Batch rebuild | Sequence append | Sequence rebuild | Rebuild / append (sequence medians) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key in ("single", "partial", "multi"):
        rows = batches[key]
        steps = len({row["step"] for row in rows})
        merges = sorted({int(row["merges"]) for row in sequences[key]})
        maintain = [int(row["maintain_ns"]) for row in rows]
        rebuild = [int(row["rebuild_ns"]) for row in rows]
        seq_maintain = [int(row["maintain_ns"]) for row in sequences[key]]
        seq_rebuild = [int(row["rebuild_ns"]) for row in sequences[key]]
        print(f"| {key} | {only({row['added_rows'] for row in rows})} | {steps} "
              f"| {', '.join(map(str, merges))} | {cell(maintain, 1)} | {cell(rebuild, 1)} "
              f"| {cell(seq_maintain, 1)} | {cell(seq_rebuild, 1)} "
              f"| {median(seq_rebuild) / median(seq_maintain):.1f} |")
    print()
    print("Batch appends split by whether the index compacted during the call: median ns [min, max]")
    print("| Scenario | Calls without merge | Append without merge | Calls with merge | "
          "Append with merge |")
    print("|---|---:|---:|---:|---:|")
    for key in ("single", "partial", "multi"):
        plain = [int(row["maintain_ns"]) for row in batches[key] if row["merges"] == "0"]
        merged = [int(row["maintain_ns"]) for row in batches[key] if row["merges"] != "0"]
        print(f"| {key} | {len(plain)} | {cell(plain, 1) if plain else 'n/a'} "
              f"| {len(merged)} | {cell(merged, 1) if merged else 'n/a'} |")
    print()


def main():
    results = Path(__file__).resolve().parent.parent / "results"
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scan", type=Path, default=results / "scan-calibration-final.csv")
    parser.add_argument("--append", type=Path, default=results / "append-maintenance.csv")
    parser.add_argument("--ordering", type=Path, default=results / "ordering-pruning.csv")
    args = parser.parse_args()

    times, blocks = load_scan(args.scan)
    print(f"== {args.scan.name}")
    print_equality(times, blocks, 8)
    print_equality(times, blocks, 1)
    print_ranges(times, blocks, (1, 4, 8))
    print_discovery(times, blocks)
    print(f"== {args.append.name}")
    print_append(args.append)
    print(f"== {args.ordering.name}")
    print_ordering(args.ordering)


if __name__ == "__main__":
    main()
