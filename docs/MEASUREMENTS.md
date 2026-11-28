# Measurements

Every number in this document comes from a raw file in [results/](../results/), except the
environment values and the compile line, which were read as described below. The tables are
printed by
`python3 bench/summarize_results.py`, and the cost-model coefficients by
`python3 bench/fit_costs.py results/scan-calibration.csv`, both from the committed files. The
measurements describe one machine, warm caches and small fixtures. They are not general
performance claims, and no minimum speedup is expected or claimed.

## What was measured, and what was not

Measured:

- forced full, zone-map and index scans, the two candidate-discovery steps and the automatic
  mode, at worker budgets 1, 2, 4 and 8, on one time-ordered fixture (`bench/calibrate.cpp`);
- the device index's build time and logical size on that fixture;
- append with incremental maintenance against a full rebuild (`bench/bench_append.cpp`);
- one narrow timestamp range over the same generated rows in time-sorted (clustered) and
  shuffled order, serial full and zone-map scans only (`bench/bench_ordering.cpp`).

Not measured: the row store against the column store, selection-vector against fused execution,
a selectivity sweep, other orderings or predicates in the ordering experiment, Zipf-distributed
equality, other table sizes or block capacities, cold caches, and the automatic mode on any
fixture other than the one its cost model was fitted on.

## Environment

| Item | Value |
|---|---|
| CPU | 11th Gen Intel Core i7-11800H @ 2.30 GHz; 8 cores, 16 logical CPUs visible to WSL2; L2 10 MiB (8 instances), L3 24 MiB |
| Memory visible to WSL2 | 7,813 MiB (`free -m`), 2,048 MiB swap |
| OS | Ubuntu 24.04.5 LTS on WSL2, kernel 6.18.58.1-microsoft-standard-WSL2, x86-64 |
| Compiler | Ubuntu clang 18.1.3 with libstdc++ 13.3.0 and glibc 2.39 |
| OpenMP | LLVM libomp 18.1.3 (`libomp-18-dev`, `libomp5-18`) |
| Build tools | CMake 3.28.3, Ninja 1.11.1 |
| Scripts | Python 3.12.3, standard library only |

The CSV files do not embed this metadata. The values above were read on the measurement machine
with `lscpu`, `free`, `uname`, `ldd --version`, `dpkg-query` and the tools' `--version` output.

## Build flags

The benchmarks use the `release` preset: `CMAKE_BUILD_TYPE=Release` with
`CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"`, no `-march` option and no link-time optimization. In
the build's `compile_commands.json`, with the source directory shortened to `.`, a library
source is compiled as:

```text
/usr/bin/clang++-18 -I./include -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wshadow
  -Wconversion -Werror -fopenmp=libomp -o CMakeFiles/cqe.dir/src/executor.cpp.o
  -c ./src/executor.cpp
```

The benchmark sources use the same flags. Each benchmark links the separately compiled static
library `libcqe.a` and `/usr/lib/llvm-18/lib/libomp.so`. The preset files and these flags did
not change between the commits that produced the committed results and the current sources.

## Commands

```bash
cmake --preset release -DCQE_BUILD_CALIBRATION=ON -DCQE_BUILD_APPEND_BENCHMARK=ON \
  -DCQE_BUILD_ORDERING_BENCHMARK=ON
cmake --build --preset release
mkdir -p results/tmp
env OMP_DYNAMIC=FALSE OMP_NUM_THREADS=8 OMP_PROC_BIND=close OMP_PLACES=cores \
  build/release/cqe_calibrate > results/tmp/calibration.csv 2> results/tmp/calibration.log
build/release/cqe_bench_append > results/tmp/append.csv
env OMP_DYNAMIC=FALSE OMP_NUM_THREADS=8 OMP_PROC_BIND=close OMP_PLACES=cores \
  build/release/cqe_bench_ordering > results/tmp/ordering.csv 2> results/tmp/ordering.log
python3 bench/summarize_results.py --scan results/tmp/calibration.csv \
  --append results/tmp/append.csv --ordering results/tmp/ordering.csv
python3 bench/fit_costs.py results/tmp/calibration.csv
```

`results/tmp/` is ignored by git, so a new run never replaces the committed files. Without
arguments, `summarize_results.py` reads the committed files. The committed runs configured one
benchmark option at a time; enabling several does not change how any benchmark is compiled.

`cqe_calibrate` runs every scan configuration once and compares its result with the reference
before timing it. A mismatch skips that configuration's timing and makes the program exit with
status 1. It also checks the index scan for devices 0, 4 and 6 before any timing and, if that
check fails, skips all index timings and exits with status 1. `cqe_bench_ordering` checks both
scans of both orderings the same way, and also requires the reference results of the two
orderings to agree. In both programs the timed repetitions are not compared with the reference
again: each repetition's result is folded into the checksum written with the sample.
`cqe_bench_append` throws, and exits with a non-zero status, if the maintained and rebuilt tables
or a query result disagree after any batch.

## Raw files and source versions

| File | Produced by | Sources | Used for |
|---|---|---|---|
| `results/scan-calibration.csv`, `results/scan-calibration.stderr.txt` | `cqe_calibrate`, OpenMP settings above | the sources of commit `37ee4fd`, except that the strategy selector was still a placeholder that always chose a serial full scan | input of `fit_costs.py`; its `auto` rows ran the placeholder and are not used anywhere |
| `results/scan-calibration-final.csv`, `results/scan-calibration-final.stderr.txt` | `cqe_calibrate`, OpenMP settings above | commit `37ee4fd` (with the fitted selector) | every scan table below |
| `results/append-maintenance.csv` | `cqe_bench_append` | commit `dd58a8b` | the append tables below |
| `results/ordering-pruning.csv`, `results/ordering-pruning.stderr.txt` | `cqe_bench_ordering`, OpenMP settings above | the library of `379e0b0`, with `bench/bench_ordering.cpp` and its build option added | the ordering table below |

Between `37ee4fd` and `dd58a8b` the library gained append maintenance, and an index lookup now
returns a two-segment view (sorted base plus a per-device delta found in a `std::map`). The
scan timings therefore describe the index lookup of `37ee4fd`. The library of `379e0b0` is the
same as that of `dd58a8b`; that commit changed only a test. The calibration and append
benchmarks were also run on the current sources as a functional check: each exited with status 0
and passed its reference checks. Those timings are not reported here.

## Fixtures

**Scan fixture** (`bench/calibrate.cpp`): 131,072 rows in 128 full blocks of 1,024 rows. Row `i`
has `timestamp = i`, `device_id = 0` for even `i` and `9` for odd `i`, except row 1,025, which
has device 4, and `amount = (i mod 17) - 8`. The column data is 131,072 × 20 bytes = 2.5 MiB, so
it fits in the last-level cache. The rows are in timestamp order, and no other ordering was
measured.

| Scenario | Predicate | Zone-map candidates | Index candidates |
|---|---|---:|---:|
| `range_k`, k in {1, 4, 16, 64, 128} | `0 <= timestamp <= k × 1024 - 1` | k | n/a |
| `range_0` | `0 <= timestamp <= -1` (an empty range) | 0 | n/a |
| `device_common` | `device_id == 0` (every block) | 128 | 128 |
| `device_rare` | `device_id == 4` (one row, block 1) | 128 | 1 |
| `device_absent` | `device_id == 6` (no row) | 128 | 0 |

Every block's device range is `[0, 9]` (block 1 also holds 4), so the zone map cannot exclude any
block for an equality predicate; only the index can.

**Append fixture** (`bench/bench_append.cpp`): the table starts with 32,755 rows (32 blocks of
1,024 rows minus 13, so the last block is partial). Initial row `i` has `device_id = i / 1024`.
Appended rows draw `device_id` from 97 ids, and all rows draw `amount` from [-15, 15], from one
`std::mt19937` stream with seed 37; `timestamp` is the row number. Both the maintained table and
the rebuilt table consume the same batches.

| Scenario | Rows per batch | Batches per sequence |
|---|---:|---:|
| `single` | 1 | 128 |
| `partial` | 257 | 32 |
| `multi` | 2,305 | 16 |

**Ordering fixture** (`bench/bench_ordering.cpp`): `generate_events` with 1,048,576 rows, seed 7
and the default configuration (1,000 uniformly distributed devices, timestamp steps in [0, 3]
from 1,700,000,000,000, amounts in [-1,000,000, 1,000,000]), in two orderings: `TimeSorted`
(clustered: timestamps non-decreasing, so each block covers a narrow time span) and `Shuffled`
(a Fisher-Yates permutation of the same rows from the same seeded stream). Blocks hold 1,024
rows, giving 1,024 blocks and 20 MiB of column data. The predicate is
`1700000787873 <= timestamp <= 1700000789458`, the timestamps of rows 524,288 and 525,335 in
time order; it matches 1,048 rows (0.0999%). The reference result, COUNT 1,048 and SUM
-19,998,005, was computed independently on each ordering's rows and was the same for both.

## Timing method

**Scans (`cqe_calibrate`):**

- Clock: `std::chrono::steady_clock`. One untimed warm-up call per configuration, which also
  creates the OpenMP team.
- Repetitions start at 1 and double until one batch of calls takes at least 5 ms. Then five
  samples are timed with that repetition count.
- Before every call an empty `asm volatile` statement with a `"memory"` clobber takes the column
  data pointers as inputs, so the compiler cannot hoist or merge the repeated calls. Each result
  passes through another empty `asm volatile` statement and is added to a checksum that is
  written with the sample.
- Before a scan configuration is timed, one untimed call's result is compared with the reference
  evaluator, and before any timing the index scan is checked for devices 0, 4 and 6
  (`index_verified=yes` in the `.stderr.txt` file). The discovery steps return candidate counts
  and are not compared with the reference.
- Reported value: the median over the five samples of `elapsed_ns / repetitions`, with the minimum
  and maximum of the same per-call values in brackets.
- Auto timings include zone-map candidate discovery, the index lookup for equality predicates,
  the decision and the scan.
- The worker budget is passed to each call. `OMP_NUM_THREADS=8`, `OMP_PROC_BIND=close`,
  `OMP_PLACES=cores` and `OMP_DYNAMIC=FALSE` were set for the process.
- The index build time is one unrepeated measurement of constructing the `IndexedTable` from a
  prebuilt `ZoneMappedTable`. It is reported separately from query time.
- `cqe_bench_ordering` uses the same method (warm-up, repetitions doubled to at least 5 ms,
  five samples, the same compiler barriers), with a worker budget of 1. Both scans of both
  orderings were compared with the reference before timing (`verified=yes` in its
  `.stderr.txt` file).

**Append (`cqe_bench_append`):**

- Each batch call is timed once, with no repetition and no compiler barrier. "Rebuild" constructs a
  new zone-mapped, indexed table over all rows so far and replaces the previous one, which
  includes destroying it.
- Six samples per scenario; the first is a warm-up and is not written. The order of append and
  rebuild alternates between samples.
- After every batch, outside the timed regions, the columns, the zones, the postings of all 97
  devices and an index query are compared between the maintained table, the rebuilt table and
  the reference.
- A sequence time is the sum of its measured batch calls; `summarize_results.py` checks every
  sequence row against its batch rows. Batch medians pool all individual calls, including the
  calls that grow buffers or compact the index.

## Limits

- One machine, a WSL2 virtual machine and warm caches. The scan and append fixtures fit in the
  last-level cache; the 20 MiB ordering fixture is close to its 24 MiB size. There are no
  cold-cache claims, and the host's scheduling and frequency behaviour were not controlled.
- Five samples per configuration. Several multi-worker configurations have a wide or bimodal
  spread, so the brackets matter as much as the medians.
- Only fused execution was timed.
- The ordering experiment is a single run with one predicate.
- The check that per-query times agree when the repetition count is doubled was not run, and the
  generated machine code was not inspected.
- The auto rows use the same fixture as the coefficient fit, and only five samples. They show the
  end-to-end cost of auto on that fixture, not how well the selector generalizes.
- The two calibration runs, on the same fixture and machine, give noticeably different
  multi-worker coefficients (see "Cost-model coefficients").

## Results: equality predicates

Worker budget 8, median nanoseconds per query [min, max]:

| Device | Index candidates | Full | Zone map | Index | Auto | Blocks scanned by auto |
|---|---:|---:|---:|---:|---:|---:|
| common | 128 | 24243 [23208, 24354] | 25051 [24496, 25811] | 25357 [25160, 26174] | 25581 [25117, 26403] | 128 |
| rare | 1 | 26277 [25397, 26629] | 29368 [28511, 30506] | 848 [812, 871] | 1702 [1095, 2075] | 1 |
| absent | 0 | 26448 [25376, 26938] | 25264 [25193, 26575] | 26 [19, 32] | 361 [350, 522] | 0 |

Worker budget 1:

| Device | Index candidates | Full | Zone map | Index | Auto | Blocks scanned by auto |
|---|---:|---:|---:|---:|---:|---:|
| common | 128 | 78937 [72906, 93496] | 82793 [82276, 92346] | 82399 [80936, 86054] | 76811 [75394, 77517] | 128 |
| rare | 1 | 78134 [74613, 89252] | 106658 [103901, 141812] | 737 [665, 984] | 1235 [1128, 1411] | 1 |
| absent | 0 | 103732 [82758, 152834] | 141295 [121104, 156276] | 23 [19, 33] | 407 [386, 432] | 0 |

- **Common device: flat to slightly negative.** The index lists all 128 blocks, so it saves
  nothing. At budget 8 the forced index scan (25357 ns) and auto (25581 ns) were 4.6% and 5.5%
  slower than the forced full scan (24243 ns). At budget 1, auto (76811 ns) and the full scan
  (78937 ns) are within each other's spread.
- **Rare device.** The index scan reads one block: 848 ns against 26277 ns for the full scan at
  budget 8. Auto chose the one-block index scan, and its time (1702 ns) includes a zone-map pass
  over all 128 blocks, which alone took a median of 387 ns.
- **Absent device.** The index proves that no block can match (26 ns). Auto took 361 ns, about
  the cost of the zone-map pass measured on its own (384 ns). The zone map cannot help, because
  6 lies inside every block's range.

## Results: pruning and serial against parallel scans

Timestamp ranges on time-ordered rows, median nanoseconds per query [min, max], at worker
budgets 1, 4 and 8:

| Scenario | Zone-map blocks | Full, 1 | Full, 4 | Full, 8 | Zone map, 1 | Zone map, 4 | Zone map, 8 |
|---|---:|---:|---:|---:|---:|---:|---:|
| range_0 | 0 | 106481 [98018, 124600] | 28116 [26731, 35947] | 27508 [25333, 42597] | 250 [242, 316] | 234 [210, 252] | 424 [235, 452] |
| range_1 | 1 | 90518 [90010, 93644] | 25168 [25011, 25798] | 24778 [24611, 24848] | 991 [985, 1063] | 1012 [992, 1020] | 1376 [1375, 1380] |
| range_16 | 16 | 97874 [95358, 119899] | 27221 [26248, 27989] | 28575 [15562, 30390] | 12841 [12726, 13122] | 4931 [4903, 5103] | 6750 [6623, 6998] |
| range_128 | 128 | 102408 [100844, 114927] | 28629 [28520, 30831] | 31298 [29740, 31521] | 115355 [114184, 117177] | 29070 [28625, 29168] | 31040 [30264, 31732] |

`summarize_results.py` also prints `range_4` and `range_64`.

- **Pruning pays when it excludes blocks.** On time-ordered rows a one-block range took 991 ns
  serially against 90518 ns for the serial full scan; an empty range skipped every block.
- **Without pruning, the zone-map path costs more.** With all 128 blocks as candidates, the
  serial zone-map scan (115355 ns) was slower than the serial contiguous full scan (102408 ns).
  The zone-map path runs the block executor, and the serial full scan does not.
- **Parallel work needs enough blocks.** For 16 candidate blocks, a budget of 4 brought the
  zone-map scan from 12841 ns to 4931 ns, and a budget of 8 was slower (6750 ns). With one
  candidate block only one worker runs, yet the budget-8 median (1376 ns) was higher than the
  serial one (991 ns); this was not investigated.
- **4 workers against 8.** Over all 16 forced and auto configurations that scan 128 blocks, a
  budget of 4 was 3.2 to 5.8 times faster than a budget of 1, apart from the three bimodal cases
  below. A budget of 8 was between 2% faster and 19% slower than a budget of 4 in 13 of the 16
  configurations. In the other three (`device_rare` full and zone map, `range_128` auto), the
  4-worker samples were bimodal, for example 26233 to 48412 ns for the `device_rare` full scan.
- **An unexplained auto result.** For `range_128` at budget 1, the cost model picks the serial
  full scan (predicted 84256 ns against 104904 ns for a serial zone-map scan), so auto should cost
  about one forced serial full scan plus a zone-map pass (655 ns). It took 147193 ns
  [145623, 148531] against 102408 ns for the forced full scan. The gap was consistent across the
  five samples and was not investigated.

## Results: clustered against shuffled rows

Source: `results/ordering-pruning.csv`. The same 1,048,576 rows and one narrow timestamp range,
worker budget 1, median nanoseconds per query [min, max]:

| Ordering | Scan | Blocks scanned | Rows examined | Rows matched | Time |
|---|---|---:|---:|---:|---:|
| clustered | full | 1024 / 1024 | 1048576 | 1048 | 735759 [701039, 744046] |
| clustered | zone map | 2 / 1024 | 2048 | 1048 | 3482 [3477, 3562] |
| shuffled | full | 1024 / 1024 | 1048576 | 1048 | 780421 [766535, 787640] |
| shuffled | zone map | 1024 / 1024 | 1048576 | 1048 | 813585 [774439, 844139] |

- **Clustered rows prune.** The matching rows lie in 2 blocks; the zone-map scan examined 2,048
  rows and took 3482 ns, against 735759 ns for the full scan.
- **Shuffled rows do not.** Every block's timestamp range overlapped the predicate, so no block
  could be skipped. The zone-map scan, which adds a metadata pass and runs the block executor,
  examined every row and was 4% slower than the full scan (813585 against 780421 ns).
- Both scans of both orderings returned the reference result. This is one predicate, one size
  and one run; it shows the mechanism, not a general ratio.

## Results: candidate discovery

Serial discovery alone, median nanoseconds per call [min, max]:

| Scenario | Zone-map candidates | Zone-map pass | Index candidates | Index lookup |
|---|---:|---:|---:|---:|
| device_common | 128 | 344 [328, 400] | 128 | 3 [3, 3] |
| device_rare | 128 | 387 [376, 670] | 1 | 4 [4, 6] |
| device_absent | 128 | 384 [363, 533] | 0 | 4 [4, 4] |
| range_0 | 0 | 215 [203, 359] | n/a | n/a |
| range_1 | 1 | 241 [238, 259] | n/a | n/a |
| range_128 | 128 | 655 [366, 671] | n/a | n/a |

`summarize_results.py` also prints `range_4`, `range_16` and `range_64`. The zone-map pass reads
all 128 zones and allocates its candidate vector; the index lookup returns borrowed spans. These
timings are from `37ee4fd`, whose lookup did not consult a delta.

## Results: index build time and size

From the `.stderr.txt` file of each calibration run, for 131,072 rows in 128 blocks of 1,024 rows:

| Run | Build time (ns, one measurement) | Logical bytes |
|---|---:|---:|
| `scan-calibration` | 2729434 | 2104 |
| `scan-calibration-final` | 2545912 | 2104 |

The 2,104 logical bytes are 3 directory entries of 16 bytes and 257 block ids of 8 bytes
(128 for device 0, 128 for device 9 and 1 for device 4). The column data of the same table is
2,621,440 bytes. Building the index creates and sorts one `(device, block)` pair per row; a
lookup is a binary search in the three-entry directory.

## Results: append with maintenance against a full rebuild

Source: `results/append-maintenance.csv` (commit `dd58a8b`). Median nanoseconds [min, max]:

| Scenario | Rows per batch | Batches | Merges per sequence | Append, per batch | Rebuild, per batch | Append, sequence | Rebuild, sequence | Rebuild / append (sequence medians) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| single | 1 | 128 | 1 | 530.5 [128.0, 123142.0] | 458746.0 [418850.0, 1674737.0] | 165884.0 [143648.0, 182639.0] | 80919685.0 [80009487.0, 86446206.0] | 487.8 |
| partial | 257 | 32 | 8 | 22222.5 [19604.0, 42983.0] | 777574.0 [461461.0, 2497257.0] | 736652.0 [714520.0, 746269.0] | 25558626.0 [25162088.0, 27176960.0] | 34.7 |
| multi | 2305 | 16 | 11 | 87554.0 [72911.0, 254811.0] | 1751654.5 [653489.0, 3047044.0] | 1489852.0 [1463213.0, 1660229.0] | 28428075.0 [28086660.0, 28970974.0] | 19.1 |

Batch appends split by whether the index compacted its delta during the call:

| Scenario | Calls without merge | Append without merge | Calls with merge | Append with merge |
|---|---:|---:|---:|---:|
| single | 635 | 524.0 [128.0, 123142.0] | 5 | 4405.0 [4186.0, 4764.0] |
| partial | 120 | 22051.5 [19807.0, 33842.0] | 40 | 23032.0 [19604.0, 42983.0] |
| multi | 25 | 85761.0 [73249.0, 101934.0] | 55 | 88380.0 [72911.0, 254811.0] |

- A rebuild costs work proportional to the whole table, an append work proportional to the
  batch, so the advantage shrinks as batches grow: from 487.8 times for single rows to 19.1 times
  for batches of 2,305 rows. The table starts at 32,755 rows and grows to at most 69,635 rows
  (`multi`).
- A single-row append that compacted the index took a median 4405 ns, against 524 ns without a
  merge. The five largest single-row times (74529 to 123142 ns) are the first append of each
  sample, the call that has to grow the column buffers.
- These are warm in-memory measurements on one small fixture. Rebuild here includes copying the
  rows into a new column store.

## Cost-model coefficients

The selector's constants in `src/strategy.cpp` are the output of `fit_costs.py` on
`results/scan-calibration.csv` (sources of `37ee4fd` with the placeholder selector; the fit uses
only forced scans and discovery timings, never auto rows). The same script applied to
`results/scan-calibration-final.csv` gives the second column, which is shown only to make the
run-to-run variation visible; the source does not use it.

| Coefficient | From `scan-calibration.csv` (used) | From `scan-calibration-final.csv` |
|---|---:|---:|
| Serial contiguous full scan, ns per block | 658.25 | 700.03 |
| 1 worker: setup ns, ns per block | 0.00, 819.56 | 0.00, 817.23 |
| 2 workers: setup ns, ns per block | 1278.88, 350.88 | 1147.84, 364.54 |
| 4 workers: setup ns, ns per block | 1085.69, 260.72 | 1305.06, 192.95 |
| 8 workers: setup ns, ns per block | 2566.59, 182.35 | 3479.81, 183.47 |

The 4-worker dense anchor moved from 34458 ns to 26002 ns between the runs. In the fitting run,
8 workers beat 4 at 128 blocks; in the final run, 4 workers were faster in most 128-block
configurations. A selector fitted on five samples of one run inherits that noise.
