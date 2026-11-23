#include "cqe/executor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include "cqe/block_layout.hpp"

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

}  // namespace cqe
