#include "cqe/reference.hpp"

#include <variant>

namespace cqe {

// Reminder: if a predicate kind is added, the reference must learn it explicitly.
static_assert(std::variant_size_v<Predicate> == 2);

QueryResult reference_evaluate(std::span<const Event> rows, const Query& query) {
  QueryResult result;
  for (const Event& row : rows) {
    bool match = false;
    if (const auto* range = std::get_if<TimestampRange>(&query.predicate)) {
      match = range->lo <= row.timestamp && row.timestamp <= range->hi;
    } else if (const auto* equals = std::get_if<DeviceEquals>(&query.predicate)) {
      match = row.device_id == equals->value;
    }
    if (match) {
      ++result.count;
      result.sum += row.amount;
    }
  }
  return result;
}

}  // namespace cqe
