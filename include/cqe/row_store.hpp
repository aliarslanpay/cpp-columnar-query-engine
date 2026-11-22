#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cqe/sum_bound.hpp"
#include "cqe/types.hpp"

namespace cqe {

// Row store: the table as one contiguous array of rows (array of structs).
// It only holds data; queries run through the executor (executor.hpp).
class RowStore {
 public:
  // Copies `rows`, keeping their order.
  // Throws std::invalid_argument if the rows violate the amount-total invariant (sum_bound.hpp).
  explicit RowStore(std::span<const Event> rows);

  // Special members keep the amount totals describing the rows at all times.
  // - Copy construction is all-or-nothing: if an allocation fails, no object exists.
  // - Move construction leaves the source as a valid empty table (no rows, zero totals). The
  //   implicit one would leave the source's old totals next to its emptied rows.
  // - Assignment is not provided: nothing needs it yet, and memberwise assignment can break the
  //   invariant (a failed allocation in copy assignment would leave new totals with old rows).
  //   If it becomes necessary, implement it with copy-and-swap for the strong guarantee.
  RowStore(const RowStore&) = default;
  RowStore(RowStore&& other) noexcept;
  RowStore& operator=(const RowStore&) = delete;
  RowStore& operator=(RowStore&&) = delete;
  ~RowStore() = default;

  [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }
  [[nodiscard]] std::span<const Event> rows() const noexcept { return rows_; }
  [[nodiscard]] AmountTotals amount_totals() const noexcept { return amount_totals_; }

 private:
  // Declared before rows_ so that the invariant is checked before any row is copied.
  AmountTotals amount_totals_;
  std::vector<Event> rows_;
};

}  // namespace cqe
