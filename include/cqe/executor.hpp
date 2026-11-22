#pragma once

#include "cqe/column_store.hpp"
#include "cqe/row_store.hpp"
#include "cqe/types.hpp"

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

}  // namespace cqe
