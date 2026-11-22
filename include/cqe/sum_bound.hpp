#pragma once

#include <cstdint>
#include <span>

#include "cqe/types.hpp"

namespace cqe {

// The amount-total invariant: the overflow contract for SUM(amount).
//
// For a table T, let P(T) be the sum of its positive amounts and N(T) the sum of its negative
// amounts. Every table held by a store satisfies
//
//     P(T) <= INT64_MAX   and   N(T) >= INT64_MIN.
//
// Every sum over any subset of T's rows, in any order and with any grouping, lies in [N(T), P(T)].
// So int64_t accumulation never overflows: not the final total, not a running partial sum, not a
// per-thread partial and not the combination of partials. The empty table has P = N = 0.
//
// The invariant is a property of the rows, not of their source. Stores check it on construction,
// whether the rows come from the generator, from hand-written fixtures or from anywhere else. An
// append must compute the new totals with extend_amount_totals() before changing the store; on
// failure it throws and the store keeps its old rows and totals.
struct AmountTotals {
  std::int64_t positive = 0;  // P(T), always >= 0
  std::int64_t negative = 0;  // N(T), always <= 0

  friend bool operator==(const AmountTotals&, const AmountTotals&) = default;
};

// Returns `totals` extended by the amounts of `rows`.
// Throws std::invalid_argument if either total would leave the int64_t range. `totals` is taken by
// value, so the caller's copy is never modified.
// Precondition: totals.positive >= 0 and totals.negative <= 0.
[[nodiscard]] AmountTotals extend_amount_totals(AmountTotals totals, std::span<const Event> rows);

}  // namespace cqe
