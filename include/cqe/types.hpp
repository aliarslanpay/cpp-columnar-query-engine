#pragma once

#include <cstdint>
#include <variant>

namespace cqe {

// One row of the `events` table. All columns are fixed-width integers.
struct Event {
  std::int64_t timestamp = 0;
  std::int32_t device_id = 0;
  std::int64_t amount = 0;  // may be negative

  friend bool operator==(const Event&, const Event&) = default;
};

// Inclusive range predicate on `timestamp`: lo <= timestamp <= hi.
// lo > hi is a valid predicate that matches no row; it is not an error.
struct TimestampRange {
  std::int64_t lo = 0;
  std::int64_t hi = 0;
};

// Equality predicate on `device_id`: device_id == value.
struct DeviceEquals {
  std::int32_t value = 0;
};

using Predicate = std::variant<TimestampRange, DeviceEquals>;

// A query computes COUNT(*) and SUM(amount), together, over the rows that satisfy exactly one
// predicate. Queries are plain C++ objects; there is no SQL.
struct Query {
  Predicate predicate;
};

// Result of a query.
//
// Empty result: count == 0 and sum == 0. This differs from SQL, where SUM over no rows is NULL;
// the prototype has no NULLs. A non-empty result can also have sum == 0 (amounts +7 and -7), so
// `count`, not `sum`, says whether any row matched.
//
// Overflow: `sum` is accumulated in int64_t. The amount-total invariant (sum_bound.hpp) guarantees
// that no sum over any subset of a stored table's rows, in any order, leaves the int64_t range.
struct QueryResult {
  std::int64_t count = 0;
  std::int64_t sum = 0;

  friend bool operator==(const QueryResult&, const QueryResult&) = default;
};

}  // namespace cqe
