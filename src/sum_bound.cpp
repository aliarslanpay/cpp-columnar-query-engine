#include "cqe/sum_bound.hpp"

#include <limits>
#include <stdexcept>

namespace cqe {

AmountTotals extend_amount_totals(AmountTotals totals, std::span<const Event> rows) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

  for (const Event& row : rows) {
    const std::int64_t amount = row.amount;
    if (amount > 0) {
      // kMax - amount cannot overflow for amount > 0.
      if (totals.positive > kMax - amount) {
        throw std::invalid_argument(
            "amount-total invariant violated: the sum of positive amounts exceeds INT64_MAX");
      }
      totals.positive += amount;
    } else if (amount < 0) {
      // kMin - amount cannot overflow for amount < 0 (it lies in [kMin + 1, 0]).
      if (totals.negative < kMin - amount) {
        throw std::invalid_argument(
            "amount-total invariant violated: the sum of negative amounts is below INT64_MIN");
      }
      totals.negative += amount;
    }
  }
  return totals;
}

}  // namespace cqe
