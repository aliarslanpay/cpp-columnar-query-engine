#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cqe/sum_bound.hpp"
#include "cqe/types.hpp"

namespace cqe {

// Column store: one contiguous array per column (struct of arrays).
// Class invariant: the three columns have the same length, row_count(), and position i in each
// column belongs to the same row. It only holds data; queries run through the executor
// (executor.hpp).
class ColumnStore {
 public:
  // Transposes `rows` into columns, keeping their order.
  // Throws std::invalid_argument if the rows violate the amount-total invariant (sum_bound.hpp).
  explicit ColumnStore(std::span<const Event> rows);

  // Special members keep both invariants (equal column lengths; amount totals describing the
  // rows) at all times.
  // - Copy construction is all-or-nothing: if an allocation fails, no object exists.
  // - Move construction leaves the source as a valid empty table (empty columns, zero totals).
  //   The implicit one would leave the source's old totals next to its emptied columns.
  // - Assignment is not provided: nothing needs it yet, and memberwise assignment can break both
  //   invariants (a failed allocation part-way through copy assignment would leave columns of
  //   different lengths and totals that describe other rows). If it becomes necessary, implement
  //   it with copy-and-swap for the strong guarantee.
  ColumnStore(const ColumnStore&) = default;
  ColumnStore(ColumnStore&& other) noexcept;
  ColumnStore& operator=(const ColumnStore&) = delete;
  ColumnStore& operator=(ColumnStore&&) = delete;
  ~ColumnStore() = default;

  [[nodiscard]] std::size_t row_count() const noexcept { return timestamp_.size(); }
  [[nodiscard]] std::span<const std::int64_t> timestamps() const noexcept { return timestamp_; }
  [[nodiscard]] std::span<const std::int32_t> device_ids() const noexcept { return device_id_; }
  [[nodiscard]] std::span<const std::int64_t> amounts() const noexcept { return amount_; }
  [[nodiscard]] AmountTotals amount_totals() const noexcept { return amount_totals_; }

 private:
  friend class ZoneMappedTable;
  // Declared first so that the invariant is checked before any column is filled.
  AmountTotals amount_totals_;
  std::vector<std::int64_t> timestamp_;
  std::vector<std::int32_t> device_id_;
  std::vector<std::int64_t> amount_;
};

}  // namespace cqe
