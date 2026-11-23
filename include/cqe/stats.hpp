#pragma once

#include <cstdint>

#include "cqe/types.hpp"

namespace cqe {

// Execution statistics returned alongside a result.
//
// Invariants for a block-based execution over a table of n rows in B blocks:
//   - blocks_total == B, and blocks_scanned + blocks_skipped == blocks_total;
//   - rows_examined is the total size of the scanned blocks, so rows_examined <= n; the rows of
//     skipped blocks are never counted as examined;
//   - rows_matched == result.count, and rows_matched <= rows_examined;
//   - blocks_scanned == blocks_total implies rows_examined == n;
//   - blocks_scanned == 0 implies rows_examined == 0 and an empty result.
// The empty table has every counter at 0. (The chosen strategy is added with auto mode.)
struct ExecutionStats {
  std::uint64_t blocks_total = 0;
  std::uint64_t blocks_scanned = 0;
  std::uint64_t blocks_skipped = 0;
  std::uint64_t rows_examined = 0;
  std::uint64_t rows_matched = 0;

  friend bool operator==(const ExecutionStats&, const ExecutionStats&) = default;
};

struct Execution {
  QueryResult result;
  ExecutionStats stats;

  friend bool operator==(const Execution&, const Execution&) = default;
};

}  // namespace cqe
