#include "cqe/column_store.hpp"

#include <utility>

namespace cqe {

ColumnStore::ColumnStore(ColumnStore&& other) noexcept
    : amount_totals_(std::exchange(other.amount_totals_, AmountTotals{})),
      timestamp_(std::move(other.timestamp_)),
      device_id_(std::move(other.device_id_)),
      amount_(std::move(other.amount_)) {
  // Moved-from vectors are only "valid but unspecified"; make the source the empty table that
  // its zero totals describe.
  other.timestamp_.clear();
  other.device_id_.clear();
  other.amount_.clear();
}

ColumnStore::ColumnStore(std::span<const Event> rows)
    : amount_totals_(extend_amount_totals(AmountTotals{}, rows)) {
  timestamp_.reserve(rows.size());
  device_id_.reserve(rows.size());
  amount_.reserve(rows.size());
  for (const Event& row : rows) {
    timestamp_.push_back(row.timestamp);
    device_id_.push_back(row.device_id);
    amount_.push_back(row.amount);
  }
}

}  // namespace cqe
