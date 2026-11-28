# Design

This document records the design decisions of the Columnar Query-Execution Prototype, the
alternatives that were rejected, and the arguments the correctness of each component rests on.
Header comments in `include/cqe/` state the exact contracts; this text explains them.

## Data model and queries

The only table is `events(timestamp int64, device_id int32, amount int64)`. A `Query` holds one
`Predicate`, a `std::variant` of `TimestampRange{lo, hi}` (inclusive) and `DeviceEquals{value}`.
Every execution returns `COUNT(*)` and `SUM(amount)` together (`QueryResult`).

- An empty result is `{0, 0}`. SQL would return `NULL` for the sum; the prototype has no NULLs,
  so `count`, not `sum`, says whether any row matched.
- `lo > hi` is a valid predicate that matches nothing.

### The amount-total invariant

`SUM` accumulates in `int64_t`. Rather than bounding `row_count * max|amount|`, every store keeps
the exact condition under which no subset sum can overflow: the sum of the positive amounts
`P(T) <= INT64_MAX` and the sum of the negative amounts `N(T) >= INT64_MIN`. Every sum over any
subset of rows, in any order and grouping, lies in `[N(T), P(T)]`. This covers running sums,
per-block partials, per-thread partials and their combination, which is what lets the parallel
reduction ignore overflow entirely.

`extend_amount_totals()` (`sum_bound.hpp`) is the single enforcement point. Stores call it on
construction and append calls it before changing anything, so a violating append throws and
leaves the table unchanged. The rejected alternative, a `n * max|amount|` bound, rejects valid
tables and is awkward to maintain under append. The data generator additionally applies a
conservative a-priori bound before generating; that bound implies the invariant.

### Reference evaluator

`reference_evaluate()` is a plain row-wise loop over the rows as generated. It depends only on
`types.hpp` and shares no code with storage, metadata, indexes or kernels, so a bug in shared code
cannot appear in both the result and the expected value. It does not check the invariant (that
check is storage code); the invariant is its precondition, and UBSan reports a violation in the
sanitizer build.

## Storage

- `RowStore` holds a vector of `Event` (array of structs); `ColumnStore` holds one vector per
  column (struct of arrays). Both full scans use one loop template in `executor.cpp`, so a layout
  comparison differs only in data layout. The match is computed with `&` on two comparisons and
  folded into the aggregates without an `if`; this describes the source only, the generated
  machine code has not been inspected.
- Special members keep the invariants: copy construction is all-or-nothing, move construction
  leaves the source as a valid empty table with zero totals (the implicit move would leave the old
  totals next to emptied vectors), and copy and move assignment are deleted because a memberwise
  assignment that fails part-way could leave columns of different lengths. The same rules apply to
  `ZoneMap`, `ZoneMappedTable`, `DeviceBlockIndex` and `IndexedTable`.

### Blocks

`BlockLayout` divides `[0, row_count)` into fixed-capacity blocks (default 1,024 rows); the last
block may be partial and the empty table has no blocks, so no block is ever empty. Blocks are
ranges over the existing contiguous columns. Block `b` ends at `begin + min(capacity, row_count -
begin)`, and the block count is `n / cap + (n % cap != 0)`: neither expression forms a sum or
product that could overflow, which the tests check with `row_count = SIZE_MAX`.

## Zone maps and pruning safety

`ZoneMap` stores, per block, the exact attained minimum and maximum of `timestamp` and
`device_id`. A partial last block uses only its own rows, and no sentinel values are needed.

The skip decision is `zone_disjoint(zone_min, zone_max, lo, hi)` (`block_skip.hpp`), with an exact
"if and only if" contract: it returns `true` exactly when no integer lies in both closed intervals.

- An inverted predicate (`lo > hi`) is empty, so every block is skipped.
- Otherwise the result is `hi < zone_min || zone_max < lo`. Two non-empty closed integer intervals
  intersect iff each starts no later than the other ends, so intervals that touch in one endpoint
  intersect and the block stays a candidate.
- Only comparisons are used, so `INT64_MIN` and `INT64_MAX` cannot overflow.
- An equality `device_id == v` is the point interval `[v, v]`, widened losslessly to `int64_t`.

Pruning safety follows directly: a block is skipped only if its exact extrema prove that no row
can match, so there are no false negatives, while a block whose range contains the value without
containing a matching row (a false positive) is scanned and filtered. The executor applies the
exact predicate to every row of every scanned block; metadata never decides a row's membership.

The tests check this three ways: an exhaustive comparison of `zone_disjoint` with direct
enumeration on a small domain, a hand-written edge-case table (touching endpoints, nested and
point intervals, inverted predicates, the `int64_t` limits), and `expect_no_false_negatives`,
which evaluates every skipped block's rows with the reference after each zone-map execution in
the fixtures and the generated equivalence suites.

**Ownership.** `ZoneMappedTable` owns its `ColumnStore` and builds the zone map from that store.
The rejected alternative, passing a store and a zone map as two arguments, cannot be validated:
a row-count check accepts the zone map of a one-row table with timestamp 10 for a one-row table
with timestamp 100, and a query for `[100, 100]` would then skip the only matching block.

## Device block index: candidates and the row filter

`IndexedTable` owns a `ZoneMappedTable` and its `DeviceBlockIndex`, so postings always describe
the rows and layout they are used with.

**Build.** The constructor emits one `(device_id, block_id)` pair per row, sorts the pairs
lexicographically and removes adjacent duplicates with `std::unique`. Equality includes both
fields, so each device keeps each block once. The result is stored as a sorted directory of
`(device_id, begin)` entries plus one flat vector of block ids; a device's postings are the
contiguous slice up to the next directory entry. Storing the device id with every posting was
rejected as redundant.

**Candidates.** For a device, the postings are exactly the blocks that contain at least one row of
that device, strictly increasing and unique. Correctness needs a superset of the blocks with a
match; for an equality predicate this list is exactly that set, and uniqueness keeps a block
from being scanned twice.
The index scan still applies `device_id == v` to every row of each listed block, so the index is
used only to choose blocks, as the zone map is. A test compares every posting list with an
independent enumeration of the blocks found directly in the rows.

**Logical size.** `logical_bytes()` counts directory entries (including padding), delta device
keys and all block ids. It excludes container objects, tree-node overhead, spare capacity and
temporary build storage, so it is a lower bound on memory use, reported separately from query
time.

## Block execution

- **Fused mode** runs the full-scan loop on zero-copy slices of the candidate blocks.
- **Selection-vector mode** first writes the offsets of matching rows into a per-worker scratch
  buffer (`select_range`, `select_equals`) and then sums `amount` only at those offsets
  (`sum_selected`, late materialization). Offsets are `uint32_t`; the limit applies to the
  actual size of the scanned block (at most 2^32 rows), not to the configured capacity. The
  kernels write the current offset unconditionally and advance by the match result, so the source
  has no data-dependent branch; the machine code is not claimed.
- **Statistics** (`stats.hpp`) report blocks total, scanned and skipped, rows examined and
  matched. Rows of skipped blocks never count as examined, and the tests check these invariants
  after every zone-map execution.

## OpenMP reduction

`scan_blocks_parallel()` (`parallel_scan.hpp`) distributes the blocks of a `BlockScanner` and
returns the field-wise sum of the per-block partials.

1. The thread count is validated before anything else, and an empty scan returns before any
   allocation or parallel region.
2. At most `min(threads, block_count)` workers are requested. Their result slots and, in
   selection-vector mode, their scratch buffers are allocated before the region, so a failed
   allocation happens outside it and nothing inside the region allocates or throws.
3. An `omp for schedule(static)` loop assigns each block to exactly one thread. Each thread
   accumulates into a local partial and writes it once to its own 64-byte-aligned slot, so there
   are no atomics, no critical sections and no shared writes; the alignment only avoids false
   sharing.
4. The slots are combined after the region's join, which makes every write visible. If the
   runtime provides a smaller team, unused slots still hold zeros.
5. One effective worker, or a build without OpenMP, uses the serial loop.

The result does not depend on the thread count or the schedule: integer addition is associative
and commutative, and the amount-total invariant keeps every partial and every combination in
range. The tests compare whole `Execution` values (result and statistics) for 1, 2, 4 and 8
threads against the serial baseline and the reference, and one test checks that a request for
four threads really uses at least two.

**ThreadSanitizer.** TSan reports false races in OpenMP code unless the runtime tells it about
OpenMP synchronization. The `tsan` preset uses LLVM libomp with its Archer tool. Five control
checks establish that this works in the toolchain: three correctly synchronized programs must be
silent, a program with a deliberate unsynchronized counter must be reported (exit code and report
both required), and an activation check requires Archer's banner. With Archer disabled
(`OMP_TOOL=disabled`, same binaries and TSan options), TSan reported races in the correctly
synchronized programs: in two of the three in one run, and in all three in each of two other
runs. That is why the checks exist.

The deliberate race uses a `volatile` counter. Without it, the compiler reduced each thread's
loop to one load and one store, and in 12 of 500 repeated runs ThreadSanitizer did not report
the race. With every increment kept as its own load and store, it reported the race in 1,000 of
1,000 runs. `volatile` adds no synchronization, so the race stays real.

## Automatic strategy selection

`auto_scan()` discovers candidates once and reuses them:

1. It computes the zone-map candidates for any predicate and, for an equality, the index
   postings.
2. `choose_strategy()` receives only the counts: total blocks, zone-map candidates and, if
   available, index candidates. It never executes the predicate on rows to learn the match count.
3. The chosen scan runs on the candidate list already obtained; there is no second metadata pass.

Discovery and the decision are part of the timed `auto_scan` call. The zone-map pass also runs
for equality predicates, where the index usually decides; its cost is visible in the
measurements.

**Cost model.** A serial full scan costs `658.25 ns × total blocks` (the contiguous column loop).
Every other plan uses the block executor and costs `setup(w) + rate(w) × candidate blocks` for a
measured team size `w`:

| Workers | Setup (ns) | Rate (ns per block) |
|---:|---:|---:|
| 1 | 0.00 | 819.56 |
| 2 | 1278.88 | 350.88 |
| 4 | 1085.69 | 260.72 |
| 8 | 2566.59 | 182.35 |

The selector starts from the serial full scan, returns at once for an empty table, and chooses a
serial zone-map or index scan when that strategy has zero candidates. Otherwise it considers each
eligible strategy at each measured team size that fits the worker budget and does not exceed the
candidate count, and keeps a plan only if it is strictly cheaper, so ties keep the earlier plan.

**Calibration.** `bench/calibrate.cpp` times every forced strategy, the two discovery steps and
auto, for worker budgets 1, 2, 4 and 8, on a fixed 131,072-row fixture with 1,024-row blocks.
`bench/fit_costs.py results/scan-calibration.csv` reproduces the constants above: for each forced
configuration it takes the median per-call time, subtracts the separately measured discovery
time for zone-map and index scans, and fits a line through two anchors per team size (the
smallest candidate count that supports the team and the dense 128-block case), clamping a
negative intercept to zero. Range and equality observations get equal weight at each anchor.

**Limits.** The model is a heuristic for fused scans, warm caches, 1,024-row blocks and one
machine. Other capacities use the same per-block costs. The coefficients come from one run of
five samples per configuration; a second run on the same fixture gives noticeably different
multi-worker coefficients (see MEASUREMENTS.md). The auto measurements use the fitting fixture,
so they are not an independent test of the selector.

## Append-only maintenance

`ZoneMappedTable::append` and `IndexedTable::append` add rows at the end; there are no updates or
deletes.

**Invariant.** After each successful append, the columns, the amount totals, the block layout,
every zone and every device's postings equal those of a table built from scratch over the
concatenated rows. Existing full blocks keep their zones. The partial last block extends its
extrema with the incoming rows, and the first row of each new block initializes that block's zone.

**Strong exception guarantee.** All fallible work happens before anything visible changes:

1. The new amount totals are computed (and the invariant checked) first.
2. Replacement buffers for the columns and zones are prepared off to the side with geometric
   growth whenever the live buffer lacks capacity.
3. The index prepares its new delta nodes, the growth of existing delta lists, or a complete new
   base if it compacts. Index publication is the last call that can throw.
4. Publication then uses swaps and writes into reserved capacity, which do not allocate or throw.

A failed append, and an empty append, leave all rows, metadata and outstanding views unchanged.
A separate test executable wraps the global scalar `operator new` with the GNU linker's `--wrap`
option, fails each allocation of an append in turn, checks that the table and the previously
borrowed views are intact, and retries successfully on the same table, for ordinary growth, for
compaction and for appends that fit into existing capacity.

**Index delta.** The index keeps its sorted base (directory plus postings) and a per-device delta
(`std::map<int32_t, std::vector<size_t>>`). An append only touches the devices in the batch. When
the delta holds more than `max(64, base postings / 8)` postings, compaction streams the base, the
existing delta and the new additions, which are sorted by device and disjoint in block order, into
a fresh directory and posting vector in linear time and temporary space. The trade-off: lookups
consult two segments and a map, and a compaction costs one linear pass; in exchange, a typical
append costs work proportional to the batch. Rejected alternatives were copying a merged list on
each lookup and reserving the live vectors one after another before all fallible work was done.

**Exclusivity.** Append requires exclusive access to the table. Concurrent readers are safe only
while no append is in progress; there is no internal synchronization and concurrent writes are not
supported.

## PostingView lifetime

`DeviceBlockIndex::candidate_blocks()` returns a `PostingView`: two borrowed spans, the base
segment and the delta segment, iterated in order as one forward range. A lookup allocates nothing,
and the size is the sum of the two span lengths.

The view borrows the index's buffers. It stays valid across an empty append and across an append
that throws, and it expires on a successful non-empty append, on a move of the owning table and on
its destruction. The allocation-failure tests record the address of every posting a view shows
before the failing append and check afterwards that the live view still starts with exactly those
objects. Callers that need postings across an append must copy them.

## Testing and build

- GoogleTest 1.17.0 is fetched at configure time with a pinned SHA256.
- Three test executables: `cqe_tests` (storage, metadata, kernels, cost model, generator and
  equivalence suites), `cqe_parallel_tests` (OpenMP execution, index and auto scans, append,
  label `openmp`) and `cqe_append_allocation_tests` (allocation-failure injection). The `tsan`
  preset adds the five OpenMP/TSan control checks.
- Generated equivalence suites compare the full scans, the zone-map scan and every parallel mode
  and thread count with the reference over several seeds, row counts, `device_id`
  distributions (uniform and Zipf) and orderings (time-sorted, shuffled, device-sorted), and the
  block scans also over several block capacities. Index scans are compared with the reference
  for every device across all orderings and distributions, and auto scans on generated
  device-sorted data with random queries at worker budgets 1 and 4.
- The generator is deterministic for a given configuration on the documented toolchain; golden
  fingerprints pin its output. The Zipf path uses floating point, so no cross-platform bit-level
  reproducibility is claimed.
- Warnings are errors in every preset; `CQE_WARNINGS_AS_ERRORS` is off for plain builds with other
  compilers.

## Not done

- SIMD (for example AVX2) filter kernels and runtime dispatch; all kernels are scalar.
- Multi-predicate queries, other aggregates, updates, deletes, persistence and SQL.
- Concurrent appends or reads during an append.
- A selector evaluated on fixtures other than its fitting fixture, or calibrated for other block
  capacities and machines.
