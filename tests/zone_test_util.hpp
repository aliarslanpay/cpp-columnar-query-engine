#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

#include "cqe/block_layout.hpp"
#include "cqe/reference.hpp"
#include "cqe/stats.hpp"
#include "cqe/types.hpp"
#include "cqe/zone_map.hpp"
#include "cqe/zone_mapped_table.hpp"
#include "test_util.hpp"

namespace cqe {

// GoogleTest printers, found by argument-dependent lookup.
inline void PrintTo(const BlockRange& range, std::ostream* os) {
  *os << "[" << range.begin << ", " << range.end << ")";
}

inline void PrintTo(const BlockZone& zone, std::ostream* os) {
  *os << "{ts=[" << zone.timestamp_min << ", " << zone.timestamp_max << "], dev=["
      << zone.device_id_min << ", " << zone.device_id_max << "]}";
}

inline void PrintTo(const ExecutionStats& stats, std::ostream* os) {
  *os << "{blocks total=" << stats.blocks_total << " scanned=" << stats.blocks_scanned
      << " skipped=" << stats.blocks_skipped << ", rows examined=" << stats.rows_examined
      << " matched=" << stats.rows_matched << "}";
}

inline void PrintTo(const Execution& execution, std::ostream* os) {
  PrintTo(execution.result, os);
  *os << " ";
  PrintTo(execution.stats, os);
}

}  // namespace cqe

namespace cqe::test {

inline ExecutionStats stats(std::uint64_t total, std::uint64_t scanned, std::uint64_t skipped,
                            std::uint64_t examined, std::uint64_t matched) {
  return ExecutionStats{total, scanned, skipped, examined, matched};
}

// Checks the invariants documented in stats.hpp for one zone-map execution on `table`.
inline void expect_stats_invariants(const Execution& execution, const ZoneMappedTable& table,
                                    const Query& query) {
  const ExecutionStats& s = execution.stats;
  const BlockLayout& layout = table.zone_map().layout();
  const std::uint64_t row_count = table.store().row_count();

  const std::vector<std::size_t> candidates = table.zone_map().candidate_blocks(query.predicate);
  std::uint64_t candidate_rows = 0;
  for (const std::size_t b : candidates) {
    candidate_rows += layout.block(b).size();
  }

  EXPECT_EQ(s.blocks_total, layout.block_count());
  EXPECT_EQ(s.blocks_scanned + s.blocks_skipped, s.blocks_total);
  EXPECT_EQ(s.blocks_scanned, candidates.size());
  EXPECT_EQ(s.rows_examined, candidate_rows);
  EXPECT_LE(s.rows_examined, row_count);
  EXPECT_GE(execution.result.count, 0);
  EXPECT_EQ(s.rows_matched, static_cast<std::uint64_t>(execution.result.count));
  EXPECT_LE(s.rows_matched, s.rows_examined);
  if (s.blocks_scanned == s.blocks_total) {
    EXPECT_EQ(s.rows_examined, row_count);
  }
  if (s.blocks_scanned == 0) {
    EXPECT_EQ(s.rows_examined, 0U);
    EXPECT_EQ(execution.result, (QueryResult{0, 0}));
  }
}

// Candidate lists are strictly ascending (so without duplicates) and inside [0, block_count).
inline void expect_well_formed_candidates(const std::vector<std::size_t>& candidates,
                                          std::size_t block_count) {
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    EXPECT_LT(candidates[i], block_count) << "candidate " << i;
    if (i > 0) {
      EXPECT_LT(candidates[i - 1], candidates[i]) << "candidate " << i;
    }
  }
}

// Direct false-negative check: no block that the zone map rules out may hold a matching row.
// Each ruled-out block's rows are evaluated with the reference evaluator on `rows`, the vector the
// table was built from (same order). The reference shares no code with the zone map, the skip
// condition or the executor.
inline void expect_no_false_negatives(std::span<const Event> rows, const ZoneMappedTable& table,
                                      const Query& query) {
  const BlockLayout& layout = table.zone_map().layout();
  std::vector<bool> is_candidate(layout.block_count(), false);
  for (const std::size_t b : table.zone_map().candidate_blocks(query.predicate)) {
    if (b < is_candidate.size()) {
      is_candidate[b] = true;
    }
  }
  for (std::size_t b = 0; b < layout.block_count(); ++b) {
    if (is_candidate[b]) {
      continue;
    }
    const BlockRange range = layout.block(b);
    const QueryResult in_block = reference_evaluate(rows.subspan(range.begin, range.size()), query);
    if (in_block.count != 0) {
      ADD_FAILURE() << "block " << b << " " << ::testing::PrintToString(range)
                    << " was skipped but holds " << in_block.count << " matching rows for "
                    << ::testing::PrintToString(query);
    }
  }
}

}  // namespace cqe::test
