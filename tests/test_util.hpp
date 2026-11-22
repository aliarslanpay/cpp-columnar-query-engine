#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "cqe/sum_bound.hpp"
#include "cqe/types.hpp"

namespace cqe {

// GoogleTest printers, found by argument-dependent lookup.
inline void PrintTo(const QueryResult& result, std::ostream* os) {
  *os << "{count=" << result.count << ", sum=" << result.sum << "}";
}

inline void PrintTo(const AmountTotals& totals, std::ostream* os) {
  *os << "{positive=" << totals.positive << ", negative=" << totals.negative << "}";
}

inline void PrintTo(const Event& row, std::ostream* os) {
  *os << "{ts=" << row.timestamp << ", dev=" << row.device_id << ", amt=" << row.amount << "}";
}

inline void PrintTo(const Query& query, std::ostream* os) {
  if (const auto* range = std::get_if<TimestampRange>(&query.predicate)) {
    *os << "timestamp in [" << range->lo << ", " << range->hi << "]";
  } else if (const auto* equals = std::get_if<DeviceEquals>(&query.predicate)) {
    *os << "device_id == " << equals->value;
  }
}

}  // namespace cqe

namespace cqe::test {

inline constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
inline constexpr std::int32_t kInt32Min = std::numeric_limits<std::int32_t>::min();
inline constexpr std::int32_t kInt32Max = std::numeric_limits<std::int32_t>::max();

inline Query range(std::int64_t lo, std::int64_t hi) { return Query{TimestampRange{lo, hi}}; }
inline Query device(std::int32_t value) { return Query{DeviceEquals{value}}; }

// Copy of `rows` sorted by (timestamp, device_id, amount): equal multisets give equal results.
inline std::vector<Event> sorted_by_full_key(std::vector<Event> rows) {
  std::sort(rows.begin(), rows.end(), [](const Event& a, const Event& b) {
    return std::tie(a.timestamp, a.device_id, a.amount) <
           std::tie(b.timestamp, b.device_id, b.amount);
  });
  return rows;
}

// FNV-1a over the column values (not the struct bytes, which include padding).
inline std::uint64_t fingerprint(std::span<const Event> rows) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto mix = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= (value >> (8 * byte)) & 0xFFU;
      hash *= 1099511628211ULL;
    }
  };
  for (const Event& row : rows) {
    mix(static_cast<std::uint64_t>(row.timestamp));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(row.device_id)));
    mix(static_cast<std::uint64_t>(row.amount));
  }
  mix(rows.size());
  return hash;
}

// a + b, clamped to the int64 range (no signed overflow).
inline std::int64_t saturating_add(std::int64_t a, std::int64_t b) {
  if (b > 0 && a > kInt64Max - b) {
    return kInt64Max;
  }
  if (b < 0 && a < kInt64Min - b) {
    return kInt64Min;
  }
  return a + b;
}

// A non-empty range (lo <= hi), at most 1000 values wide, that lies entirely outside
// [min_ts, max_ts]: above the data if `prefer_above` and possible, otherwise below it.
// Returns std::nullopt when [min_ts, max_ts] is the whole int64 range, because then no such range
// is representable. Precondition: min_ts <= max_ts.
inline std::optional<TimestampRange> outside_data_range(std::int64_t min_ts, std::int64_t max_ts,
                                                        bool prefer_above) {
  const bool above_possible = max_ts < kInt64Max;
  const bool below_possible = min_ts > kInt64Min;
  if (above_possible && (prefer_above || !below_possible)) {
    return TimestampRange{max_ts + 1, saturating_add(max_ts, 1000)};
  }
  if (below_possible) {
    return TimestampRange{saturating_add(min_ts, -1000), min_ts - 1};
  }
  return std::nullopt;
}

// Random predicates aimed at the edges of the data: endpoints taken from actual timestamps
// (so boundaries and duplicate timestamps are hit exactly), endpoints off by one, points,
// lo > hi, ranges outside the data, the full int64 range, and equality on present, random and
// absent device ids. Uses its own random stream; nothing here is shared with the code under test.
inline std::vector<Query> random_queries(std::span<const Event> rows, std::int32_t device_count,
                                         std::uint64_t seed, std::size_t query_count) {
  std::mt19937_64 rng(seed);
  const auto pick = [&rng](std::uint64_t n) {
    return std::uniform_int_distribution<std::uint64_t>(0, n - 1)(rng);
  };

  std::int64_t min_ts = 0;
  std::int64_t max_ts = 0;
  if (!rows.empty()) {
    const auto [lowest, highest] = std::minmax_element(
        rows.begin(), rows.end(),
        [](const Event& a, const Event& b) { return a.timestamp < b.timestamp; });
    min_ts = lowest->timestamp;
    max_ts = highest->timestamp;
  }
  const auto random_ts = [&] {
    return std::uniform_int_distribution<std::int64_t>(saturating_add(min_ts, -100),
                                                       saturating_add(max_ts, 100))(rng);
  };
  const auto data_ts = [&] { return rows.empty() ? random_ts() : rows[pick(rows.size())].timestamp; };
  // Two data timestamps in ascending order. (Returned by value: std::minmax(a, b) would return
  // references to the temporaries.)
  const auto ordered_pair = [&] {
    const std::int64_t a = data_ts();
    const std::int64_t b = data_ts();
    return a <= b ? std::pair{a, b} : std::pair{b, a};
  };
  const auto nudge = [&](std::int64_t t) {
    switch (pick(3)) {
      case 0:
        return t == kInt64Min ? t : t - 1;
      case 1:
        return t;
      default:
        return t == kInt64Max ? t : t + 1;
    }
  };
  const std::array<std::int32_t, 4> absent_ids = {-1, device_count, kInt32Min, kInt32Max};

  std::vector<Query> queries;
  queries.reserve(query_count);
  for (std::size_t q = 0; q < query_count; ++q) {
    switch (pick(10)) {
      case 0: {  // both endpoints from the data
        const auto [lo, hi] = ordered_pair();
        queries.push_back(range(lo, hi));
        break;
      }
      case 1: {  // endpoints from the data, each moved by -1, 0 or +1
        const auto [lo, hi] = ordered_pair();
        queries.push_back(range(nudge(lo), nudge(hi)));
        break;
      }
      case 2: {  // lo == hi on a present timestamp
        const std::int64_t t = data_ts();
        queries.push_back(range(t, t));
        break;
      }
      case 3: {  // lo > hi
        const auto [lo, hi] = ordered_pair();
        if (lo == hi) {
          queries.push_back(hi == kInt64Max ? range(hi, hi - 1) : range(hi + 1, hi));
        } else {
          queries.push_back(range(hi, lo));
        }
        break;
      }
      case 4:  // arbitrary endpoints around the data, either order
        queries.push_back(range(random_ts(), random_ts()));
        break;
      case 5: {  // entirely outside the data
        const std::optional<TimestampRange> outside =
            outside_data_range(min_ts, max_ts, pick(2) == 0);
        // With timestamps at both int64 limits no range outside the data is representable; an
        // inverted range is then the remaining way to ask a query that matches nothing.
        queries.push_back(outside ? Query{*outside} : range(kInt64Max, kInt64Min));
        break;
      }
      case 6:  // the full int64 range
        queries.push_back(range(kInt64Min, kInt64Max));
        break;
      case 7:  // a device id that is present
        queries.push_back(device(rows.empty() ? 0 : rows[pick(rows.size())].device_id));
        break;
      case 8:  // any id in the configured domain
        queries.push_back(device(static_cast<std::int32_t>(pick(static_cast<std::uint64_t>(device_count)))));
        break;
      default:  // an id outside the domain
        queries.push_back(device(absent_ids[pick(absent_ids.size())]));
        break;
    }
  }
  return queries;
}

}  // namespace cqe::test
