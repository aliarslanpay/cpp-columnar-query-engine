#include "cqe/row_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "cqe/executor.hpp"
#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt64Max;
using test::kInt64Min;

TEST(RowStore, EmptyTable) {
  const std::vector<Event> rows;
  const RowStore store{rows};
  EXPECT_EQ(store.row_count(), 0U);
  EXPECT_TRUE(store.rows().empty());
  EXPECT_EQ(store.amount_totals(), (AmountTotals{0, 0}));
}

TEST(RowStore, KeepsRowsInOrder) {
  const std::vector<Event> rows = {{30, 1, -4}, {10, 2, 5}, {20, 1, 0}};
  const RowStore store{rows};
  ASSERT_EQ(store.row_count(), rows.size());
  EXPECT_TRUE(std::equal(rows.begin(), rows.end(), store.rows().begin(), store.rows().end()));
  EXPECT_EQ(store.amount_totals(), (AmountTotals{5, -4}));
}

TEST(RowStore, CopiesItsInput) {
  std::vector<Event> rows = {{1, 1, 1}};
  const RowStore store{rows};
  rows[0].amount = 99;
  EXPECT_EQ(store.rows()[0].amount, 1);
}

// Regression: the implicit move constructor emptied the source's rows but left its amount
// totals unchanged, so the moved-from store broke the invariant.
TEST(RowStore, MoveLeavesTheSourceAsAConsistentEmptyTable) {
  const std::vector<Event> rows = {{1, 1, 5}, {2, 2, -3}};
  RowStore source{rows};
  const RowStore target{std::move(source)};
  EXPECT_EQ(target.row_count(), 2U);
  EXPECT_EQ(target.amount_totals(), (AmountTotals{5, -3}));
  EXPECT_TRUE(std::equal(rows.begin(), rows.end(), target.rows().begin(), target.rows().end()));
  // Inspecting the moved-from state is the point of this test.
  EXPECT_EQ(source.row_count(), 0U);
  EXPECT_EQ(source.amount_totals(), (AmountTotals{0, 0}));
  EXPECT_EQ(source.amount_totals(), extend_amount_totals({}, source.rows()));
  EXPECT_EQ(full_scan(source, test::range(kInt64Min, kInt64Max)), (QueryResult{0, 0}));
}

// Compile-time regression: only the special members that preserve the invariant exist.
TEST(RowStore, OnlyInvariantPreservingSpecialMembersAreProvided) {
  static_assert(std::is_copy_constructible_v<RowStore>);
  static_assert(std::is_nothrow_move_constructible_v<RowStore>);
  static_assert(!std::is_copy_assignable_v<RowStore>);
  static_assert(!std::is_move_assignable_v<RowStore>);
  SUCCEED();
}

TEST(RowStore, CopyIsIndependentAndConsistent) {
  const std::vector<Event> rows = {{1, 1, 5}, {2, 2, -3}};
  const RowStore original{rows};
  const RowStore copy{original};
  EXPECT_EQ(copy.amount_totals(), original.amount_totals());
  EXPECT_EQ(copy.amount_totals(), extend_amount_totals({}, copy.rows()));
  EXPECT_TRUE(std::equal(original.rows().begin(), original.rows().end(), copy.rows().begin(),
                         copy.rows().end()));
  EXPECT_NE(copy.rows().data(), original.rows().data());
}

TEST(RowStore, RejectsRowsThatViolateTheAmountTotalInvariant) {
  const std::vector<Event> too_positive = {{0, 0, kInt64Max}, {1, 0, 1}};
  const std::vector<Event> too_negative = {{0, 0, kInt64Min}, {1, 0, -1}};
  EXPECT_THROW(RowStore{too_positive}, std::invalid_argument);
  EXPECT_THROW(RowStore{too_negative}, std::invalid_argument);
}

}  // namespace
}  // namespace cqe
