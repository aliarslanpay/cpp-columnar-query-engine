#include "cqe/sum_bound.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt64Max;
using test::kInt64Min;

std::vector<Event> with_amounts(std::initializer_list<std::int64_t> amounts) {
  std::vector<Event> rows;
  std::int64_t ts = 0;
  for (const std::int64_t amount : amounts) {
    rows.push_back(Event{ts++, 0, amount});
  }
  return rows;
}

TEST(AmountTotals, EmptyTableHasZeroTotals) {
  EXPECT_EQ(extend_amount_totals({}, {}), (AmountTotals{0, 0}));
}

TEST(AmountTotals, SumsPositiveAndNegativeAmountsSeparately) {
  const auto rows = with_amounts({5, -3, 0, 7, -2});
  EXPECT_EQ(extend_amount_totals({}, rows), (AmountTotals{12, -5}));
}

TEST(AmountTotals, AcceptsTotalsExactlyAtTheInt64Limits) {
  EXPECT_EQ(extend_amount_totals({}, with_amounts({kInt64Max, kInt64Min})),
            (AmountTotals{kInt64Max, kInt64Min}));
  // 4 * (INT64_MIN / 4) == INT64_MIN exactly.
  EXPECT_EQ(extend_amount_totals({}, with_amounts({kInt64Min / 4, kInt64Min / 4, kInt64Min / 4,
                                                   kInt64Min / 4})),
            (AmountTotals{0, kInt64Min}));
}

TEST(AmountTotals, RejectsPositiveTotalAboveInt64Max) {
  EXPECT_THROW((void)extend_amount_totals({}, with_amounts({kInt64Max, 1})), std::invalid_argument);
  EXPECT_THROW((void)extend_amount_totals({}, with_amounts({kInt64Max / 2 + 1, kInt64Max / 2 + 1})),
               std::invalid_argument);
}

TEST(AmountTotals, RejectsNegativeTotalBelowInt64Min) {
  EXPECT_THROW((void)extend_amount_totals({}, with_amounts({kInt64Min, -1})), std::invalid_argument);
}

TEST(AmountTotals, LargeMixedSignsAreFineWhenEachSideFits) {
  // P = INT64_MAX and N = INT64_MIN: every subset sum still fits.
  EXPECT_EQ(extend_amount_totals({}, with_amounts({kInt64Max, kInt64Min, 0})),
            (AmountTotals{kInt64Max, kInt64Min}));
}

// The shape an append will use: extending existing totals equals computing them from scratch.
TEST(AmountTotals, ExtendingExistingTotalsEqualsComputingFromScratch) {
  const auto first = with_amounts({10, -20, 30});
  const auto second = with_amounts({-1, 2, kInt64Max / 4});
  std::vector<Event> all = first;
  all.insert(all.end(), second.begin(), second.end());

  const AmountTotals incremental = extend_amount_totals(extend_amount_totals({}, first), second);
  EXPECT_EQ(incremental, extend_amount_totals({}, all));
}

TEST(AmountTotals, FailedExtensionLeavesTheCallersTotalsUsable) {
  const AmountTotals at_limit = extend_amount_totals({}, with_amounts({kInt64Max}));
  EXPECT_THROW((void)extend_amount_totals(at_limit, with_amounts({1})), std::invalid_argument);
  EXPECT_EQ(at_limit, (AmountTotals{kInt64Max, 0}));
  // A later extension that fits still succeeds from the unchanged totals.
  EXPECT_EQ(extend_amount_totals(at_limit, with_amounts({-5})), (AmountTotals{kInt64Max, -5}));
}

}  // namespace
}  // namespace cqe
