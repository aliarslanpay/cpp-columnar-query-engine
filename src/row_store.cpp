#include "cqe/row_store.hpp"

#include <utility>

namespace cqe {

RowStore::RowStore(std::span<const Event> rows)
    : amount_totals_(extend_amount_totals(AmountTotals{}, rows)), rows_(rows.begin(), rows.end()) {}

RowStore::RowStore(RowStore&& other) noexcept
    : amount_totals_(std::exchange(other.amount_totals_, AmountTotals{})),
      rows_(std::move(other.rows_)) {
  // A moved-from vector is only "valid but unspecified"; make the source the empty table that
  // its zero totals describe.
  other.rows_.clear();
}

}  // namespace cqe
