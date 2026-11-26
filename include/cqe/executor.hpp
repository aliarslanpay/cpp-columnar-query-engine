#pragma once

#include "cqe/block_index.hpp"
#include "cqe/column_store.hpp"
#include "cqe/row_store.hpp"
#include "cqe/stats.hpp"
#include "cqe/strategy.hpp"
#include "cqe/types.hpp"
#include "cqe/zone_mapped_table.hpp"

namespace cqe {

// Full-scan executors: evaluate the exact predicate on every row and aggregate the matches.
//
// Both stores use one loop template, so a row-store vs column-store comparison differs only in
// data layout, not in loop shape. In the source, the match is computed with `&` on two
// comparisons and folded into the aggregates without an `if`. This describes the source only:
// what machine code the compiler emits (branches, conditional moves, vector code) is not claimed
// here and has not been inspected.
[[nodiscard]] QueryResult full_scan(const RowStore& store, const Query& query);
[[nodiscard]] QueryResult full_scan(const ColumnStore& store, const Query& query);

// Zone-map scan: asks the table's zone map for candidate blocks, then runs the same scan loop as
// the full scans over each candidate block, on zero-copy slices of the columns. Row-level
// filtering is always exact; the zone map only decides which blocks are read.
// Statistics follow the invariants in stats.hpp. This two-argument form is the serial fused
// baseline; the ScanOptions overload below adds modes and threads.
[[nodiscard]] Execution zone_map_scan(const ZoneMappedTable& table, const Query& query);

// How a block scan processes each candidate block.
enum class ScanMode {
  // Filter and aggregate in one loop (the full-scan loop).
  Fused,
  // The filter writes a selection vector of matching row offsets (kernels.hpp); the aggregation
  // then reads `amount` only at those offsets (late materialization).
  SelectionVector,
};

struct ScanOptions {
  ScanMode mode = ScanMode::Fused;
  int threads = 1;  // >= 1; candidate blocks are scanned by up to this many threads
};

// Zone-map scan with a choice of mode and thread count. For every mode and thread count the
// result and the statistics are identical to those of the two-argument baseline.
// Candidate blocks are distributed with scan_blocks_parallel (parallel_scan.hpp). In Fused mode
// no selection scratch is allocated. In SelectionVector mode each worker gets one scratch buffer
// sized to the largest candidate block.
// Throws std::invalid_argument if options.threads < 1 or the mode is unknown. In SelectionVector
// mode, throws std::length_error if a candidate block has more than kMaxSelectableRows rows. The
// limit applies to the actual size of the blocks being scanned, not to the block capacity.
[[nodiscard]] Execution zone_map_scan(const ZoneMappedTable& table, const Query& query,
                                      const ScanOptions& options);

// Scan every block and report statistics. The default fused serial scan uses the column-store
// full scan. Other modes and thread counts use the same block executor as zone_map_scan.
[[nodiscard]] Execution full_scan(const ZoneMappedTable& table, const Query& query,
                                  const ScanOptions& options = {});

// Scan the table's postings for the supplied equality predicate, applying the predicate again
// to each row of those blocks. The ScanOptions contract is the same as for zone_map_scan.
[[nodiscard]] Execution index_scan(const IndexedTable& table, DeviceEquals predicate,
                                   const ScanOptions& options = {});

struct AutoExecution {
  Execution execution;
  ExecutionPlan plan;  // Chosen strategy and requested worker limit.
};

// Choose and execute a fused scan, reporting its plan and statistics. Candidate discovery and
// the choice are part of this call. Timestamp predicates are eligible for full and zone-map
// scans, and equality predicates can also use the index. max_threads must be >= 1.
// Builds without OpenMP choose with a worker budget of 1.
[[nodiscard]] AutoExecution auto_scan(const IndexedTable& table, const Query& query,
                                      int max_threads = 1);

}  // namespace cqe
