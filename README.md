# Columnar Query-Execution Prototype

A small C++20 in-memory columnar query-execution prototype. It studies how per-block metadata
(zone maps) and a block index let a scan skip data, when skipping stops paying off, and what
OpenMP parallel block scans cost and gain. Every execution mode is checked against an
independent row-wise reference, and the measurements are reported with their raw data and
conditions.

## What it is

- One table, `events`, with three fixed-width integer columns: `timestamp` (`int64_t`),
  `device_id` (`int32_t`) and `amount` (`int64_t`, may be negative).
- Queries compute `COUNT(*)` and `SUM(amount)` together over the rows that satisfy exactly one
  typed predicate: an inclusive timestamp range `lo <= timestamp <= hi`, or a device equality
  `device_id == v`. Queries are C++ objects; there is no SQL.
- Execution modes:
  - full scans over a row store (array of structs) and a column store (struct of arrays);
  - zone-map scans that skip blocks whose per-block min/max cannot match;
  - index scans that read only the blocks a device block index lists for a device;
  - fused (filter and aggregate in one loop) and selection-vector (filter to row offsets, then
    aggregate) block kernels, both scalar C++;
  - OpenMP block-parallel scans with per-thread partial results;
  - an automatic mode that picks full, zone-map or index scan and a worker count from
    candidate-block counts with a calibrated heuristic cost model.
- Append-only maintenance: exclusive appends extend the columns, zone maps and device index
  incrementally, with a strong exception guarantee.

## What it is not

It is not a database and makes no production-readiness claim. There is no SQL parser, no joins,
no `GROUP BY`, no NULLs, no floating-point columns, no updates or deletes, no persistence, no
concurrent writes (append is exclusive), no distributed execution and no hand-written SIMD
kernels. No continuous-integration setup is included, so no hosted CI result is claimed.

## Build and test

Verified on Ubuntu 24.04 (WSL2, x86-64) with Clang 18.1.3, CMake 3.28.3 and Ninja 1.11.1. The
presets name `clang-18` and `clang++-18` explicitly. The verification machine had these Ubuntu
24.04 packages installed; on another Ubuntu 24.04 system they can be installed with:

```bash
sudo apt install clang-18 libomp-18-dev libstdc++-13-dev cmake ninja-build python3 git
```

`libomp-18-dev` provides the LLVM OpenMP runtime and its Archer tool, which the `tsan` preset
uses. The configure step downloads GoogleTest 1.17.0 (pinned by SHA256), so it needs network
access.

Clone the repository with the URL shown under the **Code** button on its GitHub page, then run
these commands from the root of the clone:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The other presets use the same three commands with their own name:

| Preset | Build | Purpose |
|---|---|---|
| `debug` | Debug | default development build |
| `release` | `-O3 -DNDEBUG`, no `-march`, no LTO | tests and benchmarks |
| `asan-ubsan` | `-O1`, AddressSanitizer + UndefinedBehaviorSanitizer | memory and undefined-behaviour checks |
| `tsan` | `-O1`, ThreadSanitizer with LLVM OpenMP and Archer | data-race checks, plus OpenMP/TSan control programs |

Every preset builds the project code with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Werror` and writes to `build/<preset>`. The `tsan` preset adds five OpenMP/ThreadSanitizer
control checks built from four small programs. One program contains a deliberate data race;
its check passes only if ThreadSanitizer reports that race.

## Usage example

```cpp
#include <vector>

#include "cqe/dataset.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"

int main() {
  using namespace cqe;
  GeneratorConfig config;
  config.row_count = 100'000;
  const std::vector<Event> rows = generate_events(config, Ordering::TimeSorted);

  // Column store, zone map (blocks of 1,024 rows) and device block index.
  const IndexedTable table{ZoneMappedTable{rows}};
  const Query query{DeviceEquals{42}};

  // Candidate discovery, strategy choice and execution with at most 4 workers.
  const AutoExecution run = auto_scan(table, query, 4);
  // run.execution.result: COUNT and SUM; run.execution.stats: blocks scanned and skipped,
  // rows examined and matched; run.plan: the chosen strategy and worker limit.
  return run.execution.result == reference_evaluate(rows, query) ? 0 : 1;
}
```

After a `release` build, this compiles and links against the static library with:

```bash
clang++-18 -std=c++20 -O2 -fopenmp -Iinclude example.cpp build/release/libcqe.a -o example
./example
```

## Architecture

```text
include/cqe/   public headers: data model, stores, block layout, zone map, device index,
               kernels, parallel block scan, cost model, executor, generator, reference
src/           implementations (library target `cqe`)
tests/         GoogleTest suites, the allocation-failure test, OpenMP/TSan control programs
bench/         calibration and append benchmarks, cost-model fit
results/       raw benchmark output
```

Storage holds data, metadata and indexes only produce candidate blocks, the executor always
applies the exact predicate to every row of a scanned block, and the benchmarks measure. The
contract of each component is documented in its header under `include/cqe/`.

## Correctness contract

- Results are compared with a trivial row-wise reference evaluator that shares no code with
  storage, metadata, indexes or kernels.
- A block may be skipped only if no row in it can match. Scanning a block without matches is
  allowed; skipping a block with a match is a bug, and the tests check skipped blocks directly.
- An empty result is `COUNT = 0` and `SUM = 0`. This differs from SQL, where `SUM` over no rows
  is `NULL`. A range with `lo > hi` is a valid, empty query.
- Every stored table keeps the sum of its positive amounts at most `INT64_MAX` and the sum of
  its negative amounts at least `INT64_MIN`, so no partial or total sum can overflow `int64_t`.
  Construction and append reject rows that would break this.
- Parallel results and statistics do not depend on the worker count; the tests compare 1, 2, 4
  and 8 workers.
- After every successful append, the columns, zone maps and index postings equal those of a
  table built from scratch over the same rows; a failed append leaves the table unchanged.

## Measured results

All numbers below are medians of `elapsed_ns / repetitions` over five samples per
configuration, computed from the raw files in [results/](results/). They were measured on one
machine (Intel Core i7-11800H, 16 logical CPUs, WSL2) with warm caches, and describe that setup
only.

Equality predicates on a 131,072-row, time-ordered fixture (128 blocks of 1,024 rows), worker
budget 8, median nanoseconds per query (`results/scan-calibration-final.csv`):

| Device | Blocks listed by the index | Full scan | Index scan | Auto (end to end) |
|---|---:|---:|---:|---:|
| rare | 1 | 26277 | 848 | 1702 |
| common | 128 | 24243 | 25357 | 25581 |
| absent | 0 | 26448 | 26 | 361 |

- The index only helps when it excludes blocks. For the common device it lists every block, and
  the index and auto paths were slightly slower than the forced full scan.
- On the same time-ordered rows, a serial zone-map scan of 1 candidate block took 991 ns against
  90518 ns for the serial full scan. With all 128 blocks as candidates the serial zone-map scan
  (115355 ns) was slower than the serial contiguous full scan (102408 ns).
- With 128 candidate blocks, a four-worker budget delivered a 1.6–5.8× speedup over one worker across 16 benchmark configurations.
- Auto includes candidate discovery and the decision. Its timings come from the same fixture
  that the cost model was fitted on, so they are not an independent evaluation of the selector.
- Appending one row with maintenance took a median 530.5 ns per call, against 458746 ns for
  rebuilding the zone-mapped, indexed table from all of its roughly 32,800 rows
  (`results/append-maintenance.csv`). For batches of 2,305 rows the sequence total was 19.1
  times smaller than rebuilding after every batch, against 487.8 times for single rows.

**Raw files.**

- `results/scan-calibration.csv` is the input of `bench/fit_costs.py`, which reproduces the
  cost-model constants in `src/strategy.cpp`. Its `auto` rows were recorded before the selector
  was calibrated and are not used.
- `results/scan-calibration-final.csv` was recorded with the calibrated selector, before append
  maintenance was added. Index lookups now also consult the append delta, so its index-lookup
  timings describe the earlier lookup path.
- `results/append-maintenance.csv` was recorded with the sources of this version.

**Reproducing the runs.** The benchmark results are written to `results/tmp/`, which git
ignores:

```bash
cmake --preset release -DCQE_BUILD_CALIBRATION=ON -DCQE_BUILD_APPEND_BENCHMARK=ON
cmake --build --preset release
mkdir -p results/tmp
env OMP_DYNAMIC=FALSE OMP_NUM_THREADS=8 OMP_PROC_BIND=close OMP_PLACES=cores \
  build/release/cqe_calibrate > results/tmp/calibration.csv 2> results/tmp/calibration.log
build/release/cqe_bench_append > results/tmp/append.csv
python3 bench/fit_costs.py results/tmp/calibration.csv
```

`cqe_calibrate` compares each scan configuration's result with the reference once before
timing it, and exits with status 1 if any comparison fails. `cqe_bench_append` compares the
maintained table, a rebuilt table and the reference after every batch, and stops with an
exception on a mismatch.

## Limitations

- A single table with three integer columns, a single typed predicate per query, and only
  `COUNT(*)` and `SUM(amount)`.
- In memory only. Appends are exclusive: no reader may use the table during an append.
- Scalar kernels only. What machine code the compiler generates (branches, vector instructions)
  has not been inspected and is not claimed.
- The cost-model coefficients were fitted on one machine for fused scans, warm caches and
  1,024-row blocks.
- Measurements use small warm fixtures on a WSL2 virtual machine and five samples per
  configuration. They describe this setup and are not general speedup claims.
- The tests that inject allocation failures rely on the GNU linker's `--wrap` option on
  Linux x86-64.

## Not done

- AVX2 or other SIMD filter kernels and runtime dispatch.
- A row-store vs column-store benchmark, a selectivity sweep, a clustered vs shuffled ordering
  comparison, Zipf-distributed equality benchmarks, thread scaling beyond the calibration
  fixture, an independent evaluation of the automatic strategy choice, and runs at other table
  sizes or block capacities.
- An environment record written by the benchmarks themselves, and a check that per-query times
  agree when the repetition count is doubled.
- Updates, deletes, persistence, SQL and multi-predicate queries (out of scope).
