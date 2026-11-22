#include "cqe/executor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

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

}  // namespace cqe
