#pragma once

#include "cqe/column_store.hpp"
#include "cqe/row_store.hpp"
#include "cqe/stats.hpp"
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

// How a zone-map scan processes each candidate block.
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

}  // namespace cqe
