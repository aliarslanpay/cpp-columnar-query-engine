#include "cqe/executor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

#include "cqe/block_layout.hpp"
#include "cqe/kernels.hpp"
#include "cqe/parallel_scan.hpp"
#include "cqe/zone_map.hpp"

namespace cqe {
namespace {

// The one scan loop shared by both stores. `match_at(i)` evaluates the predicate on row i and
// `amount_at(i)` reads its amount. The amount-total invariant keeps every partial sum in range.
template <class MatchAt, class AmountAt>
QueryResult scan(std::size_t row_count, MatchAt match_at, AmountAt amount_at) {
  QueryResult result;
  for (std::size_t i = 0; i < row_count; ++i) {
    const bool match = match_at(i);
    result.count += match;
    result.sum += match ? amount_at(i) : 0;
  }
  return result;
}

// Per-block work of the ScanOptions overload, for one predicate type P and one mode. The blocks
// are the candidate blocks, given as row ranges computed before any parallel region.
template <class P, ScanMode Mode>
class CandidateScanner final : public BlockScanner {
 public:
  CandidateScanner(const ColumnStore& store, std::span<const BlockRange> ranges, const P& predicate,
                   std::size_t largest_block)
      : timestamps_(store.timestamps()),
        device_ids_(store.device_ids()),
        amounts_(store.amounts()),
        ranges_(ranges),
        predicate_(predicate),
        largest_block_(largest_block) {}

  [[nodiscard]] std::size_t block_count() const noexcept override { return ranges_.size(); }

  [[nodiscard]] std::size_t scratch_size() const noexcept override {
    return Mode == ScanMode::SelectionVector ? largest_block_ : 0;
  }

  [[nodiscard]] BlockPartial scan_block(std::size_t i,
                                        std::span<RowOffset> scratch) const noexcept override {
    const BlockRange range = ranges_[i];
    const std::span<const std::int64_t> block_amounts = amounts_.subspan(range.begin, range.size());
    QueryResult block;
    if constexpr (Mode == ScanMode::Fused) {
      static_cast<void>(scratch);
      block = fused_block(range, block_amounts);
    } else {
      // scratch.size() == largest_block_ >= range.size(), as select_* requires.
      const std::size_t selected = select_block(range, scratch);
      block.count = static_cast<std::int64_t>(selected);
      block.sum = sum_selected(block_amounts, scratch.first(selected));
    }
    return BlockPartial{1, range.size(), block.count, block.sum};
  }

 private:
  QueryResult fused_block(BlockRange range, std::span<const std::int64_t> block_amounts) const noexcept {
    const auto amount_at = [block_amounts](std::size_t j) { return block_amounts[j]; };
    if constexpr (std::is_same_v<P, TimestampRange>) {
      const std::span<const std::int64_t> ts = timestamps_.subspan(range.begin, range.size());
      const std::int64_t lo = predicate_.lo;
      const std::int64_t hi = predicate_.hi;
      return scan(
          range.size(),
          [ts, lo, hi](std::size_t j) -> bool {
            const std::int64_t t = ts[j];
            return (lo <= t) & (t <= hi);
          },
          amount_at);
    } else {
      static_assert(std::is_same_v<P, DeviceEquals>);
      const std::span<const std::int32_t> dev = device_ids_.subspan(range.begin, range.size());
      const std::int32_t value = predicate_.value;
      return scan(
          range.size(), [dev, value](std::size_t j) -> bool { return dev[j] == value; }, amount_at);
    }
  }

  std::size_t select_block(BlockRange range, std::span<RowOffset> scratch) const noexcept {
    if constexpr (std::is_same_v<P, TimestampRange>) {
      return select_range(timestamps_.subspan(range.begin, range.size()), predicate_.lo,
                          predicate_.hi, scratch);
    } else {
      static_assert(std::is_same_v<P, DeviceEquals>);
      return select_equals(device_ids_.subspan(range.begin, range.size()), predicate_.value,
                           scratch);
    }
  }

  std::span<const std::int64_t> timestamps_;
  std::span<const std::int32_t> device_ids_;
  std::span<const std::int64_t> amounts_;
  std::span<const BlockRange> ranges_;
  P predicate_;
  std::size_t largest_block_;
};

void validate_scan_options(const ScanOptions& options) {
  if (options.threads < 1) {
    throw std::invalid_argument("ScanOptions::threads must be >= 1");
  }
  if (options.mode != ScanMode::Fused && options.mode != ScanMode::SelectionVector) {
    throw std::invalid_argument("Unknown ScanMode");
  }
}

// The candidate IDs come from this table's layout. Prepare row ranges before entering OpenMP.
template <class Candidates>
Execution scan_candidates(const ZoneMappedTable& table, const Predicate& predicate,
                          const Candidates& candidates, const ScanOptions& options) {
  const BlockLayout& layout = table.zone_map().layout();
  std::vector<BlockRange> ranges;
  ranges.reserve(candidates.size());
  std::size_t largest_block = 0;
  for (const std::size_t b : candidates) {
    ranges.push_back(layout.block(b));
    largest_block = std::max(largest_block, ranges.back().size());
  }
  if (options.mode == ScanMode::SelectionVector && largest_block > kMaxSelectableRows) {
    throw std::length_error("A candidate block has more rows than a selection vector can address");
  }

  const BlockPartial total = std::visit(
      [&](const auto& p) {
        using P = std::decay_t<decltype(p)>;
        if (options.mode == ScanMode::Fused) {
          const CandidateScanner<P, ScanMode::Fused> scanner(table.store(), ranges, p,
                                                             largest_block);
          return scan_blocks_parallel(scanner, options.threads);
        }
        const CandidateScanner<P, ScanMode::SelectionVector> scanner(table.store(), ranges, p,
                                                                     largest_block);
        return scan_blocks_parallel(scanner, options.threads);
      },
      predicate);

  return {{total.count, total.sum},
          {layout.block_count(), total.blocks, layout.block_count() - total.blocks,
           total.rows_examined, static_cast<std::uint64_t>(total.count)}};
}

}  // namespace

QueryResult full_scan(const RowStore& store, const Query& query) {
  const std::span<const Event> rows = store.rows();
  const auto amount_at = [rows](std::size_t i) { return rows[i].amount; };
  return std::visit(
      [&](const auto& predicate) -> QueryResult {
        using P = std::decay_t<decltype(predicate)>;
        if constexpr (std::is_same_v<P, TimestampRange>) {
          const std::int64_t lo = predicate.lo;
          const std::int64_t hi = predicate.hi;
          return scan(
              rows.size(),
              [rows, lo, hi](std::size_t i) -> bool {
                const std::int64_t t = rows[i].timestamp;
                return (lo <= t) & (t <= hi);
              },
              amount_at);
        } else {
          static_assert(std::is_same_v<P, DeviceEquals>);
          const std::int32_t value = predicate.value;
          return scan(
              rows.size(), [rows, value](std::size_t i) -> bool { return rows[i].device_id == value; },
              amount_at);
        }
      },
      query.predicate);
}

QueryResult full_scan(const ColumnStore& store, const Query& query) {
  const std::span<const std::int64_t> timestamps = store.timestamps();
  const std::span<const std::int32_t> device_ids = store.device_ids();
  const std::span<const std::int64_t> amounts = store.amounts();
  const auto amount_at = [amounts](std::size_t i) { return amounts[i]; };
  return std::visit(
      [&](const auto& predicate) -> QueryResult {
        using P = std::decay_t<decltype(predicate)>;
        if constexpr (std::is_same_v<P, TimestampRange>) {
          const std::int64_t lo = predicate.lo;
          const std::int64_t hi = predicate.hi;
          return scan(
              store.row_count(),
              [timestamps, lo, hi](std::size_t i) -> bool {
                const std::int64_t t = timestamps[i];
                return (lo <= t) & (t <= hi);
              },
              amount_at);
        } else {
          static_assert(std::is_same_v<P, DeviceEquals>);
          const std::int32_t value = predicate.value;
          return scan(
              store.row_count(),
              [device_ids, value](std::size_t i) -> bool { return device_ids[i] == value; },
              amount_at);
        }
      },
      query.predicate);
}

Execution zone_map_scan(const ZoneMappedTable& table, const Query& query) {
  const ColumnStore& store = table.store();
  const BlockLayout& layout = table.zone_map().layout();
  const std::vector<std::size_t> candidates = table.zone_map().candidate_blocks(query.predicate);
  const std::span<const std::int64_t> timestamps = store.timestamps();
  const std::span<const std::int32_t> device_ids = store.device_ids();
  const std::span<const std::int64_t> amounts = store.amounts();

  Execution execution;
  execution.stats.blocks_total = layout.block_count();
  execution.stats.blocks_scanned = candidates.size();
  execution.stats.blocks_skipped = layout.block_count() - candidates.size();

  // The predicate is dispatched once per query. Each candidate block runs the shared scan loop on
  // zero-copy slices of the columns; per-block partial results are subset sums, which the
  // amount-total invariant keeps in range.
  std::visit(
      [&](const auto& predicate) {
        using P = std::decay_t<decltype(predicate)>;
        for (const std::size_t b : candidates) {
          const BlockRange range = layout.block(b);
          const std::span<const std::int64_t> block_amounts =
              amounts.subspan(range.begin, range.size());
          const auto amount_at = [block_amounts](std::size_t i) { return block_amounts[i]; };
          QueryResult block_result;
          if constexpr (std::is_same_v<P, TimestampRange>) {
            const std::span<const std::int64_t> block_timestamps =
                timestamps.subspan(range.begin, range.size());
            const std::int64_t lo = predicate.lo;
            const std::int64_t hi = predicate.hi;
            block_result = scan(
                range.size(),
                [block_timestamps, lo, hi](std::size_t i) -> bool {
                  const std::int64_t t = block_timestamps[i];
                  return (lo <= t) & (t <= hi);
                },
                amount_at);
          } else {
            static_assert(std::is_same_v<P, DeviceEquals>);
            const std::span<const std::int32_t> block_device_ids =
                device_ids.subspan(range.begin, range.size());
            const std::int32_t value = predicate.value;
            block_result = scan(
                range.size(),
                [block_device_ids, value](std::size_t i) -> bool {
                  return block_device_ids[i] == value;
                },
                amount_at);
          }
          execution.result.count += block_result.count;
          execution.result.sum += block_result.sum;
          execution.stats.rows_examined += range.size();
        }
      },
      query.predicate);
  execution.stats.rows_matched = static_cast<std::uint64_t>(execution.result.count);
  return execution;
}

Execution zone_map_scan(const ZoneMappedTable& table, const Query& query,
                        const ScanOptions& options) {
  validate_scan_options(options);
  const auto candidates = table.zone_map().candidate_blocks(query.predicate);
  return scan_candidates(table, query.predicate, candidates, options);
}

Execution full_scan(const ZoneMappedTable& table, const Query& query, const ScanOptions& options) {
  validate_scan_options(options);
  const BlockLayout& layout = table.zone_map().layout();
  const std::size_t blocks = layout.block_count();
  if (options.mode == ScanMode::Fused && options.threads == 1) {
    const QueryResult result = full_scan(table.store(), query);
    return {result, {blocks, blocks, 0, layout.row_count(),
                     static_cast<std::uint64_t>(result.count)}};
  }
  return scan_candidates(table, query.predicate, std::views::iota(std::size_t{0}, blocks), options);
}

Execution index_scan(const IndexedTable& table, DeviceEquals predicate,
                     const ScanOptions& options) {
  validate_scan_options(options);
  const auto candidates = table.device_index().candidate_blocks(predicate.value);
  return scan_candidates(table.table(), predicate, candidates, options);
}

AutoExecution auto_scan(const IndexedTable& indexed, const Query& query, int max_threads) {
  if (max_threads < 1) {
    throw std::invalid_argument("auto_scan: max_threads must be >= 1");
  }
#ifndef _OPENMP
  max_threads = 1;
#endif

  const ZoneMappedTable& table = indexed.table();
  const auto zone_candidates = table.zone_map().candidate_blocks(query.predicate);
  StrategyEstimates estimates{table.zone_map().layout().block_count(), zone_candidates.size(), {}};
  PostingView index_candidates;
  if (const auto* equals = std::get_if<DeviceEquals>(&query.predicate)) {
    index_candidates = indexed.device_index().candidate_blocks(equals->value);
    estimates.index_blocks = index_candidates.size();
  }

  const ExecutionPlan plan = choose_strategy(estimates, max_threads);
  const ScanOptions options{ScanMode::Fused, plan.threads};
  switch (plan.strategy) {
    case ScanStrategy::Full:
      return {full_scan(table, query, options), plan};
    case ScanStrategy::ZoneMap:
      return {scan_candidates(table, query.predicate, zone_candidates, options), plan};
    case ScanStrategy::Index:
      return {scan_candidates(table, query.predicate, index_candidates, options), plan};
  }
  throw std::logic_error("Unknown scan strategy");
}

}  // namespace cqe
